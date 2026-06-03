include_guard(GLOBAL)

option(
    DAS_SYNC_CODEX_SKILLS
    "Create project-local Codex skill symlinks from .claude/skills during CMake configure"
    ON
)

option(
    DAS_SYNC_CODEX_AGENTS_MD
    "Create project-local AGENTS.md symlink to CLAUDE.md during CMake configure"
    ON
)

function(das_sync_codex_skills)
    # --- AGENTS.md symlink (CLAUDE.md -> AGENTS.md) ---
    if (DAS_SYNC_CODEX_AGENTS_MD)
        set(_claude_instructions "${CMAKE_SOURCE_DIR}/CLAUDE.md")
        set(_agents_instructions "${CMAKE_SOURCE_DIR}/AGENTS.md")

        if (EXISTS "${_claude_instructions}")
            if (NOT EXISTS "${_agents_instructions}" AND NOT IS_SYMLINK "${_agents_instructions}")
                execute_process(
                    COMMAND "${CMAKE_COMMAND}" -E create_symlink "CLAUDE.md" "AGENTS.md"
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    RESULT_VARIABLE _agents_link_result
                    ERROR_VARIABLE _agents_link_error
                )

                if (_agents_link_result EQUAL 0)
                    message(STATUS "DAS Codex AGENTS linked: ${_agents_instructions} -> CLAUDE.md")
                else()
                    message(WARNING
                        "DAS Codex AGENTS symlink failed; "
                        "project-local AGENTS.md link was not created. CMake error: ${_agents_link_error}"
                    )
                endif()
            else()
                message(STATUS "DAS Codex AGENTS sync skipped existing target: ${_agents_instructions}")
            endif()
        else()
            message(STATUS "DAS Codex AGENTS sync skipped: CLAUDE.md not found")
        endif()
    endif()

    # --- Codex skills symlinks (.claude/skills/* -> .codex/skills/*) ---
    if (NOT DAS_SYNC_CODEX_SKILLS)
        return()
    endif()
    set(_project_skills_dir "${CMAKE_SOURCE_DIR}/.claude/skills")
    if (NOT IS_DIRECTORY "${_project_skills_dir}")
        message(STATUS "DAS Codex skill sync skipped: .claude/skills not found")
        return()
    endif()

    set(_codex_skills_dir "${CMAKE_SOURCE_DIR}/.codex/skills")
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
