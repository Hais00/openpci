add_executable(extgpu-native-contract native_contract.c)
target_link_libraries(extgpu-native-contract PRIVATE extgpu)
target_compile_options(extgpu-native-contract PRIVATE -Wall -Wextra -Werror)
add_test(NAME sdk-native-readonly-contract COMMAND extgpu-native-contract)

add_executable(extgpu-cpp-consumer cpp_consumer.cpp)
target_link_libraries(extgpu-cpp-consumer PRIVATE extgpu)
target_compile_options(extgpu-cpp-consumer PRIVATE -Wall -Wextra -Werror -pedantic-errors)
add_test(NAME sdk-cpp17-binary-consumer COMMAND extgpu-cpp-consumer)

add_library(extgpu-test-implementation STATIC ${EXTGPU_SDK_SOURCES})
target_include_directories(extgpu-test-implementation PUBLIC "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/sdk")
target_compile_definitions(extgpu-test-implementation PUBLIC EXTGPU_TESTING=1)
target_compile_options(extgpu-test-implementation PRIVATE -Wall -Wextra -Werror -fblocks)
target_link_libraries(extgpu-test-implementation PUBLIC "-framework IOKit" "-framework CoreFoundation")
add_executable(extgpu-sdk-fault-contract sdk_fault_contract.c)
target_link_libraries(extgpu-sdk-fault-contract PRIVATE extgpu-test-implementation)
target_compile_options(extgpu-sdk-fault-contract PRIVATE -Wall -Wextra -Werror)
add_test(NAME sdk-protocol-resource-fault-contract COMMAND extgpu-sdk-fault-contract)
set_tests_properties(sdk-native-readonly-contract sdk-cpp17-binary-consumer
  sdk-protocol-resource-fault-contract PROPERTIES TIMEOUT 20)
if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
  foreach(pair "native-contract" "cpp-consumer" "sdk-fault-contract")
    add_test(NAME sdk-rosetta-${pair}
      COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:extgpu-${pair}>)
    set_tests_properties(sdk-rosetta-${pair} PROPERTIES TIMEOUT 20)
  endforeach()
endif()

add_executable(extgpu-driver-mmio-bounds-cache driver_mmio_bounds_cache.cpp)
target_include_directories(extgpu-driver-mmio-bounds-cache PRIVATE "${PROJECT_SOURCE_DIR}/driver")
target_compile_options(extgpu-driver-mmio-bounds-cache PRIVATE -Wall -Wextra -Werror -pedantic-errors)
add_test(NAME driver-mmio-bounds-cache-policy COMMAND extgpu-driver-mmio-bounds-cache)
set_tests_properties(driver-mmio-bounds-cache-policy PROPERTIES TIMEOUT 20)
if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
  add_test(NAME driver-mmio-bounds-cache-rosetta
    COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:extgpu-driver-mmio-bounds-cache>)
  set_tests_properties(driver-mmio-bounds-cache-rosetta PROPERTIES TIMEOUT 20)
endif()

add_executable(extgpu-driver-cold-removal-ledger driver_cold_removal_ledger.cpp)
target_include_directories(extgpu-driver-cold-removal-ledger PRIVATE
  "${PROJECT_SOURCE_DIR}/driver" "${PROJECT_SOURCE_DIR}/include")
target_compile_options(extgpu-driver-cold-removal-ledger PRIVATE -Wall -Wextra -Werror -pedantic-errors)
add_test(NAME driver-cold-removal-ledger-policy COMMAND extgpu-driver-cold-removal-ledger)
set_tests_properties(driver-cold-removal-ledger-policy PROPERTIES TIMEOUT 20)

add_executable(extgpu-sdk-cold-removal-contract sdk_cold_removal_contract.c)
target_link_libraries(extgpu-sdk-cold-removal-contract PRIVATE extgpu-test-implementation)
target_compile_options(extgpu-sdk-cold-removal-contract PRIVATE -Wall -Wextra -Werror)
add_test(NAME sdk-cold-removal-fault-contract COMMAND extgpu-sdk-cold-removal-contract)
set_tests_properties(sdk-cold-removal-fault-contract PROPERTIES TIMEOUT 20)
if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
  add_test(NAME driver-cold-removal-ledger-rosetta
    COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:extgpu-driver-cold-removal-ledger>)
  add_test(NAME sdk-cold-removal-fault-rosetta
    COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:extgpu-sdk-cold-removal-contract>)
  set_tests_properties(driver-cold-removal-ledger-rosetta sdk-cold-removal-fault-rosetta PROPERTIES TIMEOUT 20)
endif()

find_package(Threads REQUIRED)
add_executable(extgpu-irq-mailbox-contract irq_mailbox_contract.c)
target_include_directories(extgpu-irq-mailbox-contract PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_options(extgpu-irq-mailbox-contract PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(extgpu-irq-mailbox-contract PRIVATE Threads::Threads)
add_test(NAME driver-irq-mailbox-contract COMMAND extgpu-irq-mailbox-contract)
set_tests_properties(driver-irq-mailbox-contract PROPERTIES TIMEOUT 20)

add_executable(extgpu-mmio-window-contract mmio_window_contract.c)
target_include_directories(extgpu-mmio-window-contract PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_options(extgpu-mmio-window-contract PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_link_libraries(extgpu-mmio-window-contract PRIVATE Threads::Threads)
add_test(NAME driver-mmio-window-ticket-contract COMMAND extgpu-mmio-window-contract)
set_tests_properties(driver-mmio-window-ticket-contract PROPERTIES TIMEOUT 20)

add_executable(extgpu-driver-recovery-broker driver_recovery_broker.cpp)
target_include_directories(extgpu-driver-recovery-broker PRIVATE
  "${PROJECT_SOURCE_DIR}/driver" "${PROJECT_SOURCE_DIR}/include")
target_compile_options(extgpu-driver-recovery-broker PRIVATE -Wall -Wextra -Werror -pedantic-errors)
target_link_libraries(extgpu-driver-recovery-broker PRIVATE Threads::Threads)
add_test(NAME driver-recovery-broker-policy COMMAND extgpu-driver-recovery-broker "${CMAKE_SOURCE_DIR}")
set_tests_properties(driver-recovery-broker-policy PROPERTIES TIMEOUT 30)
if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
  add_test(NAME driver-recovery-broker-rosetta
    COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:extgpu-driver-recovery-broker> "${CMAKE_SOURCE_DIR}")
  set_tests_properties(driver-recovery-broker-rosetta PROPERTIES TIMEOUT 30)
endif()

add_executable(extgpu-driver-irq-cancellation driver_irq_cancellation.cpp)
target_include_directories(extgpu-driver-irq-cancellation PRIVATE "${PROJECT_SOURCE_DIR}/driver")
target_compile_options(extgpu-driver-irq-cancellation PRIVATE -Wall -Wextra -Werror -pedantic-errors)
target_link_libraries(extgpu-driver-irq-cancellation PRIVATE Threads::Threads)
add_test(NAME driver-irq-cancellation-policy COMMAND extgpu-driver-irq-cancellation)
set_tests_properties(driver-irq-cancellation-policy PROPERTIES TIMEOUT 30)
if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
  add_test(NAME driver-irq-cancellation-rosetta
    COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:extgpu-driver-irq-cancellation>)
  set_tests_properties(driver-irq-cancellation-rosetta PROPERTIES TIMEOUT 30)
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME sdk-installed-third-party-consumer
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/installed_consumer.py"
    --build "${CMAKE_BINARY_DIR}" --config "$<CONFIG>"
    --work "${CMAKE_CURRENT_BINARY_DIR}/installed-consumer")
set_tests_properties(sdk-installed-third-party-consumer PROPERTIES TIMEOUT 120)

add_executable(extgpu-driver-dma-retirement driver_dma_retirement.cpp)
target_include_directories(extgpu-driver-dma-retirement PRIVATE "${PROJECT_SOURCE_DIR}/driver")
target_compile_options(extgpu-driver-dma-retirement PRIVATE -Wall -Wextra -Werror -pedantic-errors)
add_test(NAME driver-shared-dma-retirement-policy COMMAND extgpu-driver-dma-retirement)
set_tests_properties(driver-shared-dma-retirement-policy PROPERTIES TIMEOUT 20)
if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
  add_test(NAME driver-shared-dma-retirement-rosetta
    COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:extgpu-driver-dma-retirement>)
  set_tests_properties(driver-shared-dma-retirement-rosetta PROPERTIES TIMEOUT 20)
endif()

add_executable(extgpu-sdk-review-state-contract sdk_review_state_contract.c
  fixtures/gpu_teardown.c)
target_link_libraries(extgpu-sdk-review-state-contract PRIVATE extgpu-test-implementation)
target_compile_options(extgpu-sdk-review-state-contract PRIVATE -Wall -Wextra -Werror)
add_test(NAME sdk-reviewed-error-state-contract COMMAND extgpu-sdk-review-state-contract)
set_tests_properties(sdk-reviewed-error-state-contract PROPERTIES TIMEOUT 20)
if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
  add_test(NAME sdk-reviewed-error-state-rosetta
    COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:extgpu-sdk-review-state-contract>)
  set_tests_properties(sdk-reviewed-error-state-rosetta PROPERTIES TIMEOUT 20)
endif()

add_executable(extgpu-driver-ownership-recovery driver_ownership_recovery.cpp)
target_include_directories(extgpu-driver-ownership-recovery PRIVATE
  "${PROJECT_SOURCE_DIR}/driver" "${PROJECT_SOURCE_DIR}/include")
target_compile_options(extgpu-driver-ownership-recovery PRIVATE -Wall -Wextra -Werror -pedantic-errors)
add_test(NAME driver-unused-ownership-recovery-policy COMMAND extgpu-driver-ownership-recovery)
set_tests_properties(driver-unused-ownership-recovery-policy PROPERTIES TIMEOUT 20)
if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
  add_test(NAME driver-unused-ownership-recovery-rosetta
    COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:extgpu-driver-ownership-recovery>)
  set_tests_properties(driver-unused-ownership-recovery-rosetta PROPERTIES TIMEOUT 20)
endif()
