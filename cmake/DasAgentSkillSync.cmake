include_guard(GLOBAL)

option(
    DAS_SYNC_CODEX_SKILLS
    "Create project-local Codex skill symlinks from .claude/skills during CMake configure"
    ON
)

set(
    DAS_CODEX_SKILLS_DIR
    "${CMAKE_SOURCE_DIR}/.codex/skills"
    CACHE PATH
    "Project-local Codex skills directory used when DAS_SYNC_CODEX_SKILLS=ON"
)

function(das_sync_codex_skills)
    if (NOT DAS_SYNC_CODEX_SKILLS)
        return()
    endif()

    set(_project_skills_dir "${CMAKE_SOURCE_DIR}/.claude/skills")
    if (NOT IS_DIRECTORY "${_project_skills_dir}")
        message(STATUS "DAS Codex skill sync skipped: .claude/skills not found")
        return()
    endif()

    set(_codex_skills_dir "${DAS_CODEX_SKILLS_DIR}")
    file(MAKE_DIRECTORY "${_codex_skills_dir}")
    file(
        GLOB _skill_dirs
        LIST_DIRECTORIES true
        "${_project_skills_dir}/*"
    )

    foreach (_skill_dir IN LISTS _skill_dirs)
        if (NOT IS_DIRECTORY "${_skill_dir}")
            continue()
        endif()
        if (NOT EXISTS "${_skill_dir}/SKILL.md")
            continue()
        endif()

        get_filename_component(_skill_name "${_skill_dir}" NAME)
        set(_target_dir "${_codex_skills_dir}/${_skill_name}")

        if (EXISTS "${_target_dir}" OR IS_SYMLINK "${_target_dir}")
            message(STATUS "DAS Codex skill sync skipped existing target: ${_target_dir}")
            continue()
        endif()

        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E create_symlink "${_skill_dir}" "${_target_dir}"
            RESULT_VARIABLE _link_result
            ERROR_VARIABLE _link_error
        )

        if (_link_result EQUAL 0)
            message(STATUS "DAS Codex skill linked: ${_target_dir} -> ${_skill_dir}")
        else()
            message(WARNING
                "DAS Codex skill symlink failed for ${_skill_name}; "
                "project-local skill link was not created. CMake error: ${_link_error}"
            )
        endif()
    endforeach()
endfunction()
