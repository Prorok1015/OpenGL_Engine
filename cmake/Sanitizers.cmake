# SNK_SANITIZE: "" / none | address (ASan + UBSan) | thread (TSan)
# Flags are carried by the INTERFACE target `snk_sanitizers`, which is linked only by
# project targets (snk::common and the executables). For address, third-party code (assimp,
# glfw, spdlog, yaml, boost) is deliberately NOT instrumented; see .github/sanitizers/.
# TSan needs the whole program instrumented: libstdc++ inlines differ under -fsanitize=thread,
# and an uninstrumented COMDAT copy winning the link hides atomics from TSan (#532). The
# ci-linux-tsan preset therefore also puts -fsanitize=thread into CMAKE_<LANG>_FLAGS.
set(SNK_SANITIZE "" CACHE STRING "Sanitizer build: none, address (ASan+UBSan), thread (TSan)")
set_property(CACHE SNK_SANITIZE PROPERTY STRINGS "" none address thread)

add_library(snk_sanitizers INTERFACE)
if(SNK_SANITIZE AND NOT SNK_SANITIZE STREQUAL "none")
	if(MSVC)
		if(SNK_SANITIZE STREQUAL "address")
			target_compile_options(snk_sanitizers INTERFACE /fsanitize=address)
		else()
			message(FATAL_ERROR "SNK_SANITIZE=${SNK_SANITIZE} is not supported with MSVC")
		endif()
	else()
		if(SNK_SANITIZE STREQUAL "address")
			set(_snk_san_flags -fsanitize=address,undefined -fno-sanitize-recover=undefined)
		elseif(SNK_SANITIZE STREQUAL "thread")
			set(_snk_san_flags -fsanitize=thread)
			# GCC -Wtsan: "atomic_thread_fence is not supported with -fsanitize=thread". The only
			# fences in our TUs are Boost.Asio's (std_fenced_block), reported through inlining even
			# though the header is SYSTEM, so SYSTEM cannot silence it; it stays visible but is not an error.
			# A known TSan limitation, not actionable in project code.
			if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
				target_compile_options(snk_sanitizers INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=tsan>)
			endif()
		else()
			message(FATAL_ERROR "SNK_SANITIZE must be none, address or thread (got '${SNK_SANITIZE}')")
		endif()
		target_compile_options(snk_sanitizers INTERFACE ${_snk_san_flags} -fno-omit-frame-pointer -g)
		target_link_options(snk_sanitizers INTERFACE ${_snk_san_flags})
	endif()
	message(STATUS "Sanitizer build: ${SNK_SANITIZE}")
	if(SNK_SANITIZE STREQUAL "thread")
		message(STATUS "TSan: CMAKE_C_FLAGS='${CMAKE_C_FLAGS}' CMAKE_CXX_FLAGS='${CMAKE_CXX_FLAGS}'")
		if(NOT MSVC AND NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize=thread")
			message(WARNING "SNK_SANITIZE=thread without -fsanitize=thread in CMAKE_CXX_FLAGS: third-party code stays "
				"uninstrumented and TSan may report false races (#532). Use the ci-linux-tsan preset.")
		endif()
	endif()
endif()
add_library(snk::sanitizers ALIAS snk_sanitizers)
