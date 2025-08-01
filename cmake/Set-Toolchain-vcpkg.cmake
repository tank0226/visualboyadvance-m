if(TRANSLATIONS_ONLY)
    return()
endif()

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(NOT WIN32)
        return()
    endif()

    # Check if we are in an MSVC environment.
    find_program(cl_exe_path NAME cl.exe HINTS ENV PATH)

    if(ENV{CXX} MATCHES "cl.exe$" OR cl_exe_path)
        # Infer the architecture from the LIB folders.
        foreach(lib $ENV{LIB})
            if(lib MATCHES "x64$")
                set(VBAM_VCPKG_PLATFORM "x64-windows-static")
                break()
            endif()
            if(lib MATCHES "x86$")
                set(VBAM_VCPKG_PLATFORM "x86-windows-static")
                break()
            endif()
            if(lib MATCHES "ARM64$")
                set(VBAM_VCPKG_PLATFORM "arm64-windows-static")

                foreach(path $ENV{PATH})
                    if(path MATCHES "[Hh]ost[Xx]64")
                        set(VCPKG_HOST_TRIPLET "x64-windows" CACHE STRING "Vcpkg host triplet" FORCE)
                        set(VCPKG_USE_HOST_TOOLS ON CACHE BOOL "Use vcpkg host tools" FORCE)
                    endif()
                endforeach()

                break()
            endif()
        endforeach()

        # If all else fails, try to use a sensible default.
        if(NOT DEFINED VBAM_VCPKG_PLATFORM)
            set(VBAM_VCPKG_PLATFORM "x64-windows-static")
        endif()

        unset(cl_exe_path)
    elseif (NOT DEFINED CMAKE_CXX_COMPILER)
        # No way to infer the compiler.
        return()

    elseif(${CMAKE_CXX_COMPILER} MATCHES "clang-cl.exe$" OR ${CMAKE_CXX_COMPILER} MATCHES "clang-cl$")
        # For stand-alone clang-cl, assume x64.
        set(VBAM_VCPKG_PLATFORM "x64-windows-static")
    endif()

    if (NOT DEFINED VBAM_VCPKG_PLATFORM)
        # Probably not an MSVC environment.
        return()
    endif()

    set(VCPKG_TARGET_TRIPLET ${VBAM_VCPKG_PLATFORM} CACHE STRING "Vcpkg target triplet (ex. x64-windows-static)" FORCE)
    message(STATUS "Inferred VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}")
endif()

function(vcpkg_seconds)
    if(CMAKE_HOST_SYSTEM MATCHES Windows OR ((NOT DEFINED CMAKE_HOST_SYSTEM) AND WIN32))
        execute_process(
            COMMAND cmd /c echo %TIME:~0,8%
            OUTPUT_VARIABLE time
        )
    else()
        execute_process(
            COMMAND date +%H:%M:%S
            OUTPUT_VARIABLE time
        )
    endif()

    string(SUBSTRING "${time}" 0 2 hours)
    string(SUBSTRING "${time}" 3 2 minutes)
    string(SUBSTRING "${time}" 6 2 secs)

    math(EXPR seconds "(${hours} * 60 * 60) + (${minutes} * 60) + ${secs}")

    set(seconds ${seconds} PARENT_SCOPE)
endfunction()

function(vcpkg_check_git_status git_status)
    # The VS vcpkg component cannot be written to without elevation.
    if(NOT git_status EQUAL 0 AND NOT VCPKG_ROOT MATCHES "Visual Studio")
        message(FATAL_ERROR "Error updating vcpkg from git, please make sure git for windows is installed correctly, it can be installed from Visual Studio components")
    endif()
endfunction()

function(vcpkg_get_first_upgrade vcpkg_exe)
    # First get the list of upgraded ports.
    execute_process(
        COMMAND ${vcpkg_exe} upgrade
        OUTPUT_VARIABLE upgradable
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        WORKING_DIRECTORY ${VCPKG_ROOT}
    )

    string(REGEX REPLACE "\r?\n" ";" upgrade_lines "${upgradable}")

    unset(first_upgrade)

    foreach(line ${upgrade_lines})
        if(line MATCHES "^  [* ] [^ ]*:")
            string(REGEX REPLACE "^  [* ] ([^[]+).*" "\\1" pkg     ${line})
            string(REGEX REPLACE "^[^:]+:(.+)$"      "\\1" triplet ${line})

            if(triplet STREQUAL "${VCPKG_TARGET_TRIPLET}")
                # Prefer upgrading zlib before anything else.
                if(NOT first_upgrade OR pkg MATCHES zlib)
                    set(first_upgrade ${pkg})
                endif()
            endif()
        endif()
    endforeach()

    if(DEFINED first_upgrade)
        set(first_upgrade ${first_upgrade} PARENT_SCOPE)
    endif()
endfunction()

function(vcpkg_deps_fixup vcpkg_exe)
    # Get installed list.
    execute_process(
        COMMAND ${vcpkg_exe} list
        OUTPUT_VARIABLE pkg_list
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        WORKING_DIRECTORY ${VCPKG_ROOT}
    )

    # If libvorbis is NOT installed but libogg is, remove libvorbis recursively.
    if(pkg_list MATCHES libogg AND (NOT pkg_list MATCHES libvorbis))
        execute_process(
            COMMAND ${vcpkg_exe} remove --recurse libogg:${VCPKG_TARGET_TRIPLET}
            WORKING_DIRECTORY ${VCPKG_ROOT}
        )
    endif()
endfunction()

function(vcpkg_is_installed vcpkg_exe pkg_name pkg_ver pkg_triplet powershell outvar)
    set(${outvar} FALSE PARENT_SCOPE)

    unset(CMAKE_MATCH_1)
    string(REGEX REPLACE "-r([0-9]+)\$" "" pkg_ver ${pkg_ver})
    set(pkg_rev ${CMAKE_MATCH_1})

    string(REPLACE "-" "." pkg_ver ${pkg_ver})

    if(NOT DEFINED VCPKG_INSTALLED_COUNT)
        execute_process(
            COMMAND ${powershell}
                -executionpolicy bypass -noprofile
                -command "import-module '${CMAKE_BINARY_DIR}/vcpkg-binpkg/vcpkg-binpkg.psm1'; vcpkg-list"
            OUTPUT_VARIABLE vcpkg_list_text
        )

        string(REGEX REPLACE "\r?\n" ";" vcpkg_list_raw "${vcpkg_list_text}")

        set(VCPKG_INSTALLED_COUNT 0 CACHE INTERNAL "Number of installed vcpkg packages" FORCE)
        foreach(pkg ${vcpkg_list_raw})
            if(NOT pkg MATCHES "^([^:[]+)[^:]*:([^ ]+) +([0-9][^ ]*) +.*\$")
                continue()
            endif()
            set(inst_pkg_name    ${CMAKE_MATCH_1})
            set(inst_pkg_ver     ${CMAKE_MATCH_3})
            set(inst_pkg_triplet ${CMAKE_MATCH_2})

            unset(CMAKE_MATCH_1)
            string(REGEX REPLACE "#([0-9]+)\$" "" inst_pkg_ver ${inst_pkg_ver})
            if(CMAKE_MATCH_1)
                set(inst_pkg_rev ${CMAKE_MATCH_1})
            else()
                set(inst_pkg_rev FALSE)
            endif()

            string(REPLACE "-" "." inst_pkg_ver ${inst_pkg_ver})

            list(APPEND VCPKG_INSTALLED ${inst_pkg_name} ${inst_pkg_ver} ${inst_pkg_rev} ${inst_pkg_triplet})
            math(EXPR VCPKG_INSTALLED_COUNT "${VCPKG_INSTALLED_COUNT} + 1")
        endforeach()
        set(VCPKG_INSTALLED       ${VCPKG_INSTALLED}       CACHE INTERNAL "List of installed vcpkg packages"   FORCE)
        set(VCPKG_INSTALLED_COUNT ${VCPKG_INSTALLED_COUNT} CACHE INTERNAL "Number of installed vcpkg packages" FORCE)
    endif()

    if(NOT VCPKG_INSTALLED_COUNT GREATER 0)
        return()
    endif()

    math(EXPR idx_max "(${VCPKG_INSTALLED_COUNT} - 1) * 4")

    foreach(idx RANGE 0 ${idx_max} 4)
        math(EXPR idx_ver     "${idx} + 1")
        math(EXPR idx_rev     "${idx} + 2")
        math(EXPR idx_triplet "${idx} + 3")
        list(GET VCPKG_INSTALLED ${idx}         inst_pkg_name)
        list(GET VCPKG_INSTALLED ${idx_ver}     inst_pkg_ver)
        list(GET VCPKG_INSTALLED ${idx_rev}     inst_pkg_rev)
        list(GET VCPKG_INSTALLED ${idx_triplet} inst_pkg_triplet)

        if(NOT inst_pkg_triplet STREQUAL pkg_triplet)
            continue()
        endif()

        if(inst_pkg_name STREQUAL pkg_name
            AND pkg_ver VERSION_LESS inst_pkg_ver
            OR (pkg_ver VERSION_EQUAL inst_pkg_ver
                AND ((NOT pkg_rev AND NOT inst_pkg_rev)
                    OR (pkg_rev AND inst_pkg_rev AND (NOT pkg_rev GREATER inst_pkg_rev)))))

            set(${outvar} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

function(get_triplet_package_list triplet)
    if(EXISTS "${CMAKE_BINARY_DIR}/binary_package_list_${triplet}.html")
        return()
    endif()

    file(
        DOWNLOAD "https://nightly.visualboyadvance-m.org/vcpkg/${triplet}/" "${CMAKE_BINARY_DIR}/binary_package_list_${triplet}.html"
        STATUS pkg_list_status
    )
    list(GET pkg_list_status 1 pkg_list_error)
    list(GET pkg_list_status 0 pkg_list_status)

    if(NOT pkg_list_status EQUAL 0)
        message(STATUS "Failed to download vcpkg binary package list: ${pkg_list_status} - ${pkg_list_error}")
        return()
    endif()
endfunction()

function(download_package pkg pkgs_dir)
    string(REGEX REPLACE "^[^_]+_[^_]+_([^.]+)[.]zip\$" "\\1" pkg_triplet ${pkg})

    message(STATUS "Downloading https://nightly.visualboyadvance-m.org/vcpkg/${pkg_triplet}/${pkg} ...")

    file(
        DOWNLOAD "https://nightly.visualboyadvance-m.org/vcpkg/${pkg_triplet}/${pkg}" "${pkgs_dir}/${pkg}"
        STATUS pkg_download_status
    )
    list(GET pkg_download_status 1 pkg_download_error)
    list(GET pkg_download_status 0 pkg_download_status)

    if(NOT pkg_download_status EQUAL 0)
        message(STATUS "Failed to download vcpkg binary package '${pkg}': ${pkg_download_status} - ${pkg_download_error}")
        return()
    endif()

    message(STATUS "done.")
endfunction()

function(zip_is_installed vcpkg_exe zip outvar)
    if(NOT zip MATCHES "([^_]+)_([^_]+)_([^.]+)[.]zip")
        return()
    endif()
    set(pkg_name    ${CMAKE_MATCH_1})
    set(pkg_version ${CMAKE_MATCH_2})
    set(pkg_triplet ${CMAKE_MATCH_3})

    vcpkg_is_installed(${vcpkg_exe} ${pkg_name} ${pkg_version} ${pkg_triplet} ${POWERSHELL} pkg_installed)

    set(${outvar} ${pkg_installed} PARENT_SCOPE)
endfunction()

function(cleanup_binary_packages)
    file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/vcpkg-binary-packages")

    unset(VCPKG_INSTALLED       CACHE)
    unset(VCPKG_INSTALLED_COUNT CACHE)
endfunction()

function(get_binary_packages vcpkg_exe)
    set(binary_packages_installed FALSE PARENT_SCOPE)

    get_triplet_package_list(${VCPKG_TARGET_TRIPLET})

    if(NOT EXISTS "${CMAKE_BINARY_DIR}/binary_package_list_${VCPKG_TARGET_TRIPLET}.html")
        message(STATUS "Failed to download binary package list found for triplet '${VCPKG_TARGET_TRIPLET}', aborting.")
        return()
    endif()

    file(READ "${CMAKE_BINARY_DIR}/binary_package_list_${VCPKG_TARGET_TRIPLET}.html" raw_html)
    string(REGEX MATCHALL "<a href=\"[^\"]+[.]zip\"" links ${raw_html})
    foreach(link ${links})
        string(REGEX REPLACE "<a href=\"([^\"]+[.]zip)\"" "\\1" pkg ${link})
        list(APPEND binary_packages ${pkg})
    endforeach()

    set(vcpkg_binpkg_dir ${CMAKE_BINARY_DIR}/vcpkg-binpkg)
    include(FetchContent)
    FetchContent_Declare(
        vcpkg_binpkg
        URL "https://github.com/rkitover/vcpkg-binpkg-prototype/archive/refs/heads/master.zip"
        SOURCE_DIR ${vcpkg_binpkg_dir}
    )

    FetchContent_GetProperties(vcpkg_binpkg)
    if(NOT vcpkg_binpkg_POPULATED)
        FetchContent_MakeAvailable(vcpkg_binpkg)
    endif()

    foreach(pkg ${binary_packages})
        zip_is_installed(${vcpkg_exe} ${pkg} pkg_installed)

        if(NOT pkg_installed)
            list(APPEND to_install ${pkg})
        endif()
    endforeach()

    if(to_install)
        set(bin_pkgs_dir ${CMAKE_BINARY_DIR}/vcpkg-binary-packages)
        file(MAKE_DIRECTORY ${bin_pkgs_dir})

        foreach(pkg ${to_install})
            download_package("${pkg}" "${bin_pkgs_dir}")

            if(NOT EXISTS "${bin_pkgs_dir}/${pkg}")
                message(STATUS "Failed to download package '${pkg}', aborting.")
                return()
            endif()
        endforeach()

        unset(installed_host_deps)

        while(TRUE)
#                       -command "import-module ($env:USERPROFILE + '/source/repos/vcpkg-binpkg-prototype/vcpkg-binpkg.psm1'); vcpkg-listmissing ."
            execute_process(
                COMMAND ${POWERSHELL}
                    -executionpolicy bypass -noprofile
                    -command "import-module '${CMAKE_BINARY_DIR}/vcpkg-binpkg/vcpkg-binpkg.psm1'; vcpkg-listmissing ."
                WORKING_DIRECTORY ${bin_pkgs_dir}
                OUTPUT_VARIABLE host_deps
                RESULT_VARIABLE host_deps_status
            )

            if(NOT host_deps_status EQUAL 0)
                message(STATUS "Failed to calculate host dependencies: ${host_deps_status}")
                return()
            endif()

            string(REGEX REPLACE "\r?\n"   ";" host_deps "${host_deps}")
            string(REGEX REPLACE " *;+ *$" ""  host_deps "${host_deps}")

            list(LENGTH host_deps           host_deps_count)
            list(LENGTH installed_host_deps installed_host_deps_count)

            if(host_deps_count EQUAL installed_host_deps_count)
                break()
            endif()

            foreach(host_dep ${host_deps})
                if(NOT host_dep MATCHES "^([^:]+):([^:]+)\$")
                    continue()
                endif()
                set(host_dep_name    ${CMAKE_MATCH_1})
                set(host_dep_triplet ${CMAKE_MATCH_2})

                get_triplet_package_list(${host_dep_triplet})

                file(READ "${CMAKE_BINARY_DIR}/binary_package_list_${host_dep_triplet}.html" raw_html)
                string(REGEX MATCHALL "<a href=\"${host_dep_name}_[^\"]+[.]zip\"" links ${raw_html})

                list(LENGTH links links_count)

                if(NOT links_count EQUAL 1)
                    message(STATUS "Multiple host dependencies found for '${host_dep_name}' for triplet '${host_dep_triplet}', aborting.")
                    return()
                endif()

                string(REGEX REPLACE "<a href=\"([^\"]+[.]zip)\"" "\\1" pkg ${links})

                list(FIND installed_host_deps "${pkg}" found_idx)

                if(found_idx EQUAL -1)
                    zip_is_installed(${vcpkg_exe} ${pkg} pkg_installed)

                    if(NOT pkg_installed)
                        download_package("${pkg}" "${bin_pkgs_dir}")

                        if(NOT EXISTS "${bin_pkgs_dir}/${pkg}")
                            message(STATUS "Failed to download host dependency package '${pkg}', aborting.")
                            return()
                        endif()
                    else()
                        list(APPEND installed_host_deps "${pkg}")
                    endif()
                endif()
            endforeach()
        endwhile()

        execute_process(
            COMMAND ${POWERSHELL}
                -executionpolicy bypass -noprofile
                -command "import-module '${CMAKE_BINARY_DIR}/vcpkg-binpkg/vcpkg-binpkg.psm1'; vcpkg-instpkg ."
            WORKING_DIRECTORY ${bin_pkgs_dir}
        )

        file(REMOVE_RECURSE ${bin_pkgs_dir})
    endif()

    cleanup_binary_packages()

    set(binary_packages_installed TRUE PARENT_SCOPE)
endfunction()

function(vcpkg_remove_optional_deps vcpkg_exe)
    list(LENGTH VCPKG_DEPS_OPTIONAL optionals_list_len)
    math(EXPR optionals_list_last "${optionals_list_len} - 1")

    unset(deps)

    foreach(i RANGE 0 ${optionals_list_last} 2)
        list(GET VCPKG_DEPS_OPTIONAL ${i} dep)

        list(APPEND deps ${dep}:${VCPKG_TARGET_TRIPLET})
    endforeach()

    execute_process(
        COMMAND ${vcpkg_exe} remove --recurse ${deps}
        WORKING_DIRECTORY ${VCPKG_ROOT}
    )
endfunction()

function(vcpkg_set_toolchain)
    get_filename_component(preferred_root ${CMAKE_SOURCE_DIR}/../vcpkg ABSOLUTE)

    if(NOT DEFINED POWERSHELL)
        message(FATAL_ERROR "Powershell is required to use vcpkg binaries.")
    endif()
    if(NOT DEFINED ENV{VCPKG_ROOT})

        if(WIN32)
            if(DEFINED ENV{CI} OR EXISTS /vcpkg)
                set(VCPKG_ROOT /vcpkg)
            elseif(EXISTS c:/vcpkg)
                set(VCPKG_ROOT c:/vcpkg)
            # Prefer the preferred root to the VS default which is more difficult to deal with, if it exists.
            elseif(EXISTS ${preferred_root})
                set(VCPKG_ROOT ${preferred_root})
            else()
                find_program(vcpkg_exe_path NAME vcpkg.exe HINTS ENV PATH)

                if(vcpkg_exe_path)
                    get_filename_component(VCPKG_ROOT ${vcpkg_exe_path} DIRECTORY)
                    get_filename_component(VCPKG_ROOT ${VCPKG_ROOT}     ABSOLUTE)
                endif()

                unset(vcpkg_exe_path)
            endif()
        endif()

        if(NOT DEFINED VCPKG_ROOT)
            set(VCPKG_ROOT ${preferred_root})
        endif()

        set(ENV{VCPKG_ROOT} ${VCPKG_ROOT})
    else()
        set(VCPKG_ROOT $ENV{VCPKG_ROOT})
    endif()

    # Avoid using Visual Studio default vcpkg, because that requires elevaction.
    if(VCPKG_ROOT MATCHES "Visual Studio")
        set(mkdir_status 0)
        if(NOT EXISTS "${preferred_root}")
            file(
                MAKE_DIRECTORY "${preferred_root}"
                RESULT mkdir_status
            )
        endif()

        if(mkdir_status EQUAL 0)
            set(VCPKG_ROOT "${preferred_root}")
            set(ENV{VCPKG_ROOT} ${VCPKG_ROOT})
        endif()
    endif()

    set(VCPKG_ROOT ${VCPKG_ROOT} CACHE FILEPATH "vcpkg installation root path" FORCE)

    if(NOT EXISTS ${VCPKG_ROOT})
        get_filename_component(root_parent ${VCPKG_ROOT}/.. ABSOLUTE)

        execute_process(
            COMMAND git clone https://github.com/microsoft/vcpkg.git
            RESULT_VARIABLE git_status
            WORKING_DIRECTORY ${root_parent}
        )

        vcpkg_check_git_status(${git_status})
    else()
        # this is the case when we cache vcpkg/installed with the appveyor build cache
        if(NOT EXISTS ${VCPKG_ROOT}/.git)
            set(git_commands
                "git init"
                "git remote add origin https://github.com/microsoft/vcpkg.git"
                "git fetch --all --prune"
                "git reset --hard origin/master"
                "git branch --set-upstream-to=origin/master master"
            )
            foreach(git_command ${git_commands})
                separate_arguments(git_command)

                execute_process(
                    COMMAND ${git_command}
                    RESULT_VARIABLE git_status
                    WORKING_DIRECTORY ${VCPKG_ROOT}
                )

                vcpkg_check_git_status(${git_status})
            endforeach()
        else()
            execute_process(
                COMMAND git fetch origin
                RESULT_VARIABLE git_status
                WORKING_DIRECTORY ${VCPKG_ROOT}
            )
            vcpkg_check_git_status(${git_status})

            execute_process(
                COMMAND git status
                RESULT_VARIABLE git_status
                OUTPUT_VARIABLE git_status_text
                WORKING_DIRECTORY ${VCPKG_ROOT}
            )
            vcpkg_check_git_status(${git_status})

            set(git_up_to_date FALSE)

            if(git_status_text MATCHES "Your branch is up to date with")
                set(git_up_to_date TRUE)
            endif()

            if(NOT git_up_to_date)
                execute_process(
                    COMMAND git pull --rebase
                    RESULT_VARIABLE git_status
                    WORKING_DIRECTORY ${VCPKG_ROOT}
                )

                vcpkg_check_git_status(${git_status})
            endif()
        endif()
    endif()

    # build latest vcpkg, if needed
    if(NOT git_up_to_date)
        if(WIN32)
            execute_process(
                COMMAND bootstrap-vcpkg.bat
                WORKING_DIRECTORY ${VCPKG_ROOT}
            )
        else()
            execute_process(
                COMMAND ./bootstrap-vcpkg.sh
                WORKING_DIRECTORY ${VCPKG_ROOT}
            )
        endif()
    endif()

    foreach(pkg ${VCPKG_DEPS})
        list(APPEND VCPKG_DEPS_QUALIFIED ${pkg}:${VCPKG_TARGET_TRIPLET})

        if(VCPKG_TARGET_TRIPLET STREQUAL "x86-mingw-static")
            list(APPEND VCPKG_DEPS_QUALIFIED libsamplerate:x86-mingw-static)
        endif()
    endforeach()

    if(WIN32)
        set(vcpkg_exe "${VCPKG_ROOT}/vcpkg.exe")
    else()
        set(vcpkg_exe "${VCPKG_ROOT}/vcpkg")
    endif()

    if (NOT (NO_VCPKG_UPDATES OR (NOT VCPKG_BINARY_PACKAGES)))
        get_binary_packages(${vcpkg_exe})
    endif()

    if(NOT binary_packages_installed)
        # Get number of seconds since midnight (might be wrong if am/pm is in effect on Windows.)
        vcpkg_seconds()
        set(began ${seconds})

        # Limit total installation time to 20 minutes to not overrun CI time limit.
        math(EXPR time_limit "${began} + (20 * 60)")

        vcpkg_deps_fixup("${vcpkg_exe}")

        # Install core deps.
        execute_process(
            COMMAND ${vcpkg_exe} --triplet ${VCPKG_TARGET_TRIPLET} install ${pkg}
            WORKING_DIRECTORY ${VCPKG_ROOT}
        )

        # If ports have been updated, and there is time, rebuild cache one at a time to not overrun the CI time limit.
        vcpkg_seconds()

        if(seconds LESS time_limit)
            vcpkg_get_first_upgrade(${vcpkg_exe})

            if(DEFINED first_upgrade)
                # If we have to upgrade zlib, remove optional deps first so that
                # the build doesn't overrun the CI time limit.
                if(first_upgrade STREQUAL "zlib")
                    vcpkg_remove_optional_deps(${vcpkg_exe})
                endif()

                execute_process(
                    COMMAND ${vcpkg_exe} upgrade --no-dry-run "${first_upgrade}:${VCPKG_TARGET_TRIPLET}"
                    WORKING_DIRECTORY ${VCPKG_ROOT}
                )
            endif()
        endif()

        # Install optional deps, within time limit.
        list(LENGTH VCPKG_DEPS_OPTIONAL optionals_list_len)
        math(EXPR optionals_list_last "${optionals_list_len} - 1")

        foreach(i RANGE 0 ${optionals_list_last} 2)
            list(GET VCPKG_DEPS_OPTIONAL ${i} dep)

            math(EXPR var_idx "${i} + 1")

            list(GET VCPKG_DEPS_OPTIONAL ${var_idx} var)
            set(val "${${var}}")

            vcpkg_seconds()

            if("${val}" OR (seconds LESS time_limit AND ("${val}" OR "${val}" STREQUAL "")))
                set(dep_qualified "${dep}:${VCPKG_TARGET_TRIPLET}")

                execute_process(
                    COMMAND --triplet ${VCPKG_TARGET_TRIPLET} ${vcpkg_exe} install ${dep}
                    WORKING_DIRECTORY ${VCPKG_ROOT}
                )

                set(${var} ON)
            else()
                set(${var} OFF)
            endif()
        endforeach()
    endif()

    if(WIN32 AND VCPKG_TARGET_TRIPLET MATCHES x64 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(CMAKE_GENERATOR_PLATFORM x64 CACHE STRING "visual studio build architecture" FORCE)
    endif()

    if(WIN32 AND NOT CMAKE_GENERATOR MATCHES "Visual Studio" AND NOT DEFINED CMAKE_CXX_COMPILER)
        if(VCPKG_TARGET_TRIPLET MATCHES "-windows-")
            # set toolchain to VS for e.g. Ninja or jom
            set(CMAKE_C_COMPILER   cl CACHE STRING "Microsoft C/C++ Compiler" FORCE)
            set(CMAKE_CXX_COMPILER cl CACHE STRING "Microsoft C/C++ Compiler" FORCE)
        endif()
    endif()

    set(CMAKE_TOOLCHAIN_FILE ${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake CACHE FILEPATH "vcpkg toolchain" FORCE)
endfunction()

vcpkg_set_toolchain()

include(${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake)
