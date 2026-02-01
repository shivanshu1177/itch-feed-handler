function(target_enable_sanitizers target)
    option(ITCH_SANITIZER "Build with a specific sanitizer" "")

    if(NOT ITCH_SANITIZER)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        set(SANITIZER_FLAG "-fsanitize=${ITCH_SANITIZER}")

        if(ITCH_SANITIZER STREQUAL "address")
            set(SANITIZER_FLAG "${SANITIZER_FLAG},undefined")
        elseif(ITCH_SANITIZER STREQUAL "thread")
            set(SANITIZER_FLAG "${SANITIZER_FLAG},undefined")
        elseif(ITCH_SANITIZER STREQUAL "memory")
            set(SANITIZER_FLAG "-fsanitize=memory -fno-omit-frame-pointer -fsanitize-memory-track-origins=2")
        endif()

        target_compile_options(${target} PRIVATE ${SANITIZER_FLAG})
        target_link_options(${target} PRIVATE ${SANITIZER_FLAG})
    else()
        message(WARNING "Sanitizer ${ITCH_SANITIZER} not supported for compiler ${CMAKE_CXX_COMPILER_ID}")
    endif()
endfunction()

