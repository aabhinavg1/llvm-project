add_flang_unittest(FlangRuntimeTests
  AccessTest.cpp
  Allocatable.cpp
  ArrayConstructor.cpp
  BufferTest.cpp
  CharacterTest.cpp
  CommandTest.cpp
  Complex.cpp
  CrashHandlerFixture.cpp
  Derived.cpp
  ExternalIOTest.cpp
  Format.cpp
  Inquiry.cpp
  ListInputTest.cpp
  LogicalFormatTest.cpp
  Matmul.cpp
  MatmulTranspose.cpp
  MiscIntrinsic.cpp
  Namelist.cpp
  Numeric.cpp
  NumericalFormatTest.cpp
  Pointer.cpp
  Ragged.cpp
  Random.cpp
  Reduction.cpp
  RuntimeCrashTest.cpp
  Stop.cpp
  Support.cpp
  Time.cpp
  TemporaryStack.cpp
  Transformational.cpp
)

target_link_libraries(FlangRuntimeTests
  PRIVATE
  flang_rt.runtime
)

target_compile_definitions(FlangRuntimeTests PRIVATE NOT_EXE="$<TARGET_FILE:not>")

add_subdirectory(CUDA)
