add_executable(x2recomp
  src/main.cpp
  src/pe.cpp
  src/discover.cpp
  src/lift.cpp
  src/lift_vec.cpp)
target_link_libraries(x2recomp PRIVATE capstone::capstone)
target_compile_options(x2recomp PRIVATE -Wall -Wextra -Wno-unused-parameter)
