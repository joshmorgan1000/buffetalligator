#!/bin/bash

# Zero-copy compliance checker for cswift library
# This script checks for prohibited memory copy operations

set -euo pipefail

# Color codes for output
RED='\033[0;31m'
YELLOW='\033[1;33m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "🔍 Zero-Copy Compliance Check for cswift"
echo "========================================"

# Counters
VIOLATIONS=0
WARNINGS=0
ALLOWED=0

# Check for prohibited operations
check_file() {
    local file="$1"
    local violations_in_file=0
    local warnings_in_file=0
    
    # Skip if file doesn't exist
    [[ ! -f "$file" ]] && return 0
    
    echo "Checking: $file"
    
    # Check for memcpy violations
    while IFS= read -r line; do
        if [[ -n "$line" ]]; then
            line_number=$(echo "$line" | cut -d: -f1)
            prev_line_number=$((line_number - 1))
            
            # Check if current line or previous 2 lines have ZERO_COPY_ALLOWED
            prev2_line_number=$((line_number - 2))
            if [[ "$line" =~ ZERO_COPY_ALLOWED ]] || \
               (sed -n "${prev_line_number}p" "$file" 2>/dev/null | grep -q "ZERO_COPY_ALLOWED") || \
               (sed -n "${prev2_line_number}p" "$file" 2>/dev/null | grep -q "ZERO_COPY_ALLOWED"); then
                echo -e "  ${YELLOW}ALLOWED${NC}: $line"
                ((ALLOWED++))
            else
                echo -e "  ${RED}VIOLATION${NC}: $line"
                ((violations_in_file++))
            fi
        fi
    done < <(grep -n "memcpy\|std::copy\|memmove" "$file" || true)
    
    # Check for copy assignment operators (warnings)
    if grep -n "operator=.*const.*&" "$file" | grep -v "delete\|ZERO_COPY_ALLOWED"; then
        while IFS= read -r line; do
            if [[ "$line" =~ ZERO_COPY_ALLOWED ]]; then
                echo -e "  ${YELLOW}ALLOWED${NC}: $line"
                ((ALLOWED++))
            else
                echo -e "  ${YELLOW}WARNING${NC}: Potential copy assignment: $line"
                ((warnings_in_file++))
            fi
        done < <(grep -n "operator=.*const.*&" "$file" || true)
    fi
    
    # Check for vector/string copies in hot paths
    if grep -n "std::vector.*=" "$file" | grep -v "move\|ZERO_COPY_ALLOWED"; then
        while IFS= read -r line; do
            if [[ "$line" =~ ZERO_COPY_ALLOWED ]]; then
                echo -e "  ${YELLOW}ALLOWED${NC}: $line"
                ((ALLOWED++))
            else
                echo -e "  ${YELLOW}WARNING${NC}: Potential vector copy: $line"
                ((warnings_in_file++))
            fi
        done < <(grep -n "std::vector.*=" "$file" || true)
    fi
    
    # Check Swift copyMemory operations  
    while IFS= read -r line; do
        if [[ -n "$line" ]]; then
            line_number=$(echo "$line" | cut -d: -f1)
            prev_line_number=$((line_number - 1))
            
            # Check if current line or previous 2 lines have ZERO_COPY_ALLOWED
            prev2_line_number=$((line_number - 2))
            if [[ "$line" =~ ZERO_COPY_ALLOWED ]] || \
               (sed -n "${prev_line_number}p" "$file" 2>/dev/null | grep -q "ZERO_COPY_ALLOWED") || \
               (sed -n "${prev2_line_number}p" "$file" 2>/dev/null | grep -q "ZERO_COPY_ALLOWED"); then
                echo -e "  ${YELLOW}ALLOWED${NC}: Swift memory copy: $line"
                ((ALLOWED++))
            else
                echo -e "  ${RED}VIOLATION${NC}: Swift memory copy: $line"
                ((violations_in_file++))
            fi
        fi
    done < <(grep -n "copyMemory\|copyBytes" "$file" || true)
    
    VIOLATIONS=$((VIOLATIONS + violations_in_file))
    WARNINGS=$((WARNINGS + warnings_in_file))
    
    if [[ $violations_in_file -eq 0 && $warnings_in_file -eq 0 ]]; then
        echo -e "  ${GREEN}✓ Clean${NC}"
    fi
    
    echo
}

# Check all source files
echo "Checking C++ sources..."
for file in $(find "$PROJECT_ROOT/src/cpp" -name "*.cpp" -o -name "*.hpp"); do
    check_file "$file"
done

echo "Checking C bridge sources..."
for file in $(find "$PROJECT_ROOT/src/bridge" -name "*.c" -o -name "*.h"); do
    check_file "$file"
done

echo "Checking Swift sources..."
for file in $(find "$PROJECT_ROOT/src/swift" -name "*.swift"); do
    check_file "$file"
done

echo "Checking headers..."
for file in $(find "$PROJECT_ROOT/include" -name "*.h" -o -name "*.hpp"); do
    check_file "$file"
done

echo "Checking examples..."
for file in $(find "$PROJECT_ROOT/examples" -name "*.cpp" -o -name "*.hpp"); do
    check_file "$file"
done

echo "Checking tests..."
for file in $(find "$PROJECT_ROOT/tests" -name "*.cpp" -o -name "*.hpp"); do
    check_file "$file"
done

# Summary
echo "========================================="
echo "Zero-Copy Compliance Summary:"
echo -e "  ${RED}Violations: $VIOLATIONS${NC}"
echo -e "  ${YELLOW}Warnings: $WARNINGS${NC}"
echo -e "  ${YELLOW}Allowed: $ALLOWED${NC}"

if [[ $VIOLATIONS -gt 0 ]]; then
    echo -e "\n${RED}❌ FAILED: $VIOLATIONS zero-copy violations found${NC}"
    echo "Please fix these violations or mark them as allowed with:"
    echo "  // ZERO_COPY_ALLOWED - reason for exception"
    exit 1
elif [[ $WARNINGS -gt 0 ]]; then
    echo -e "\n${YELLOW}⚠️  WARNINGS: $WARNINGS potential issues found${NC}"
    echo "Consider reviewing these for performance impact"
    exit 0
else
    echo -e "\n${GREEN}✅ PASSED: No zero-copy violations found${NC}"
    exit 0
fi