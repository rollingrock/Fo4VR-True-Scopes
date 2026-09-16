# Deploy one built plugin into an MO2 mod folder, refusing a Debug build.
#
# The post-build copy used to run for whatever config was built. On a multi-config
# generator `cmake --build buildvr` with no --config builds Debug, so the documented
# command minus one flag silently put unoptimised code on the rig I actually play and
# measure on. FRIK hit exactly that on 2026-09-16 and it reached the test rig; the
# giveaway was a DLL four times the usual size, not anything in the game.
#
# Deploying Debug on purpose is a real workflow - it is how you attach a debugger to
# the plugin running in the game - so this refuses the accident, not the intent:
# configure with -DDEPLOY_DEBUG_BUILD=ON and it deploys, loudly. The refusal names
# the flag so the next person needing it finds it from the message.
#
# Invoked with -DCONFIG= -DDEST= -DDLL= -DPDB= -DSRC= -DALLOW_DEBUG=.

if(CONFIG STREQUAL "Debug")
	if(NOT ALLOW_DEBUG)
		message(STATUS "True Scopes: NOT deploying to ${DEST} - this is a ${CONFIG} build. "
		               "Build with --config Release, or configure with -DDEPLOY_DEBUG_BUILD=ON "
		               "if you meant to put a Debug build on the rig (debugger attach).")
		return()
	endif()
	message(WARNING "True Scopes: deploying a DEBUG build to ${DEST} because "
	                "DEPLOY_DEBUG_BUILD=ON. It is unoptimised - do not measure performance "
	                "against it, and put a Release build back before playing.")
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
