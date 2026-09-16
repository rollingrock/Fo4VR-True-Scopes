# Deploy one built plugin into an MO2 mod folder, refusing a Debug build.
#
# The post-build copy used to run for whatever config was built. On a multi-config
# generator `cmake --build buildvr` with no --config builds Debug, so the documented
# command minus one flag silently put unoptimised code on the rig I actually play and
# measure on. FRIK hit exactly that on 2026-09-16 and it reached the test rig; the
# giveaway was a DLL four times the usual size, not anything in the game.
#
# Invoked with -DCONFIG= -DDEST= -DDLL= -DPDB= -DSRC=.

if(CONFIG STREQUAL "Debug")
	message(STATUS "True Scopes: NOT deploying to ${DEST} - this is a ${CONFIG} build. "
	               "Build with --config Release to deploy.")
	return()
endif()

file(MAKE_DIRECTORY "${DEST}")
file(COPY "${DLL}" DESTINATION "${DEST}")
if(EXISTS "${PDB}")
	file(COPY "${PDB}" DESTINATION "${DEST}")
endif()
file(COPY "${SRC}/Data/F4SE/Plugins/TrueScopesVR.toml" DESTINATION "${DEST}")

# The two edited ScopeMenu SWFs ship beside the plugin (Data/Interface), so the rig
# never drifts from the repo.
get_filename_component(_interface "${DEST}/../../Interface" ABSOLUTE)
file(MAKE_DIRECTORY "${_interface}")
file(COPY "${SRC}/Data/Interface/world_ScopeMenu.swf" DESTINATION "${_interface}")
file(COPY "${SRC}/Data/Interface/ScopeMenu.swf" DESTINATION "${_interface}")

message(STATUS "True Scopes: deployed ${CONFIG} build to ${DEST}")
