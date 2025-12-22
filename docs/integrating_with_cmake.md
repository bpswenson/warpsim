# Integrating Warpsim with CMake

This repo is intended to be usable as a dependency from a separate model project.

Warpsim is a header-only library target exported as a CMake package, so downstream projects can do `find_package(warpsim CONFIG REQUIRED)` and link the target.

## Option A: Install + find_package (recommended)

Build and install Warpsim:

```bash
cmake -S /path/to/warpsim -B build
cmake --build build -j
cmake --install build --prefix /opt/warpsim
```

In your model project:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_model LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(warpsim CONFIG REQUIRED)

add_executable(my_model main.cpp)
target_link_libraries(my_model PRIVATE warpsim::warpsim)
```

Configure your model project by pointing CMake at the install prefix:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/warpsim
cmake --build build -j
```

## Option B: Add as a subdirectory (good for dev)

If you keep Warpsim as a git submodule (or a sibling directory), you can build it as part of your project:

```cmake
add_subdirectory(/path/to/warpsim warpsim-build)

target_link_libraries(my_model PRIVATE warpsim)
# or (also available in-tree)
# target_link_libraries(my_model PRIVATE warpdes)
```

The exported/imported target namespace for installed packages is:

- `warpsim::warpsim`

## Notes

- Warpsim currently exports header-only interface targets. Linking the target primarily supplies the correct include directories.
- If you need MPI for your model, that’s handled by your model’s build; Warpsim’s MPI tests and example transport are internal to this repo.
