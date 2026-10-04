if(NOT PROJECT_NAME STREQUAL "alligator" OR NOT CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    return()
endif()
enable_testing()
function(register_kitchen_dispatch_experiments)
    set(experiment "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    foreach(program IN ITEMS main burst io_isolation)
        add_executable(kitchen_dispatch_${program} "${experiment}/${program}.cpp")
        target_link_libraries(kitchen_dispatch_${program} PRIVATE alligator::alligator)
        target_compile_options(kitchen_dispatch_${program} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
    foreach(executor IN ITEMS kitchen blocking sleeping spinning)
        add_test(NAME dispatch_${executor}_read COMMAND kitchen_dispatch_main
            --executor ${executor} --mode storage --work read --depth 16
            --requests 64 --warmup 0 --repetitions 1)
        add_test(NAME dispatch_${executor}_burst COMMAND kitchen_dispatch_burst
            --executor ${executor} --producers 4 --batch 8 --tasks 256 --warmup 0
            --repetitions 1 --csv "${CMAKE_BINARY_DIR}/smoke-${executor}-burst.csv")
        foreach(path IN ITEMS shared separate coroutine)
            add_test(NAME dispatch_${executor}_${path} COMMAND kitchen_dispatch_io_isolation
                --executor ${executor} --path ${path} --rounds 2
                --csv "${CMAKE_BINARY_DIR}/smoke-${executor}-${path}.csv")
            set_tests_properties(dispatch_${executor}_${path} PROPERTIES TIMEOUT 30
                ENVIRONMENT "ALLIGATOR_GPU_BACKEND=cpu")
        endforeach()
        set_tests_properties(dispatch_${executor}_read dispatch_${executor}_burst
            PROPERTIES TIMEOUT 30 ENVIRONMENT "ALLIGATOR_GPU_BACKEND=cpu")
    endforeach()
    message(STATUS "Kitchen dispatch experiments enabled; production sources are unchanged")
endfunction()
cmake_language(DEFER CALL register_kitchen_dispatch_experiments)
