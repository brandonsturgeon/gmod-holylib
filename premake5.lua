-- Defines which version of the project generator to use, by default
-- (can be overriden by the environment variable PROJECT_GENERATOR_VERSION)
PROJECT_GENERATOR_VERSION = 2

newoption({
	trigger = "gmcommon",
	description = "Sets the path to the garrysmod_common (https://github.com/danielga/garrysmod_common) directory",
	default = "../garrysmod_common"
})

local gmcommon = assert(_OPTIONS.gmcommon or os.getenv("GARRYSMOD_COMMON"),
	"you didn't provide a path to your garrysmod_common (https://github.com/danielga/garrysmod_common) directory")
include(gmcommon)

CreateWorkspace({name = "holylib", abi_compatible = false})
	-- Serverside module (gmsv prefix)
	-- Can define "source_path", where the source files are located
	-- Can define "manual_files", which allows you to manually add files to the project,
	-- instead of automatically including them from the "source_path"
	-- Can also define "abi_compatible", for project specific compatibility
	CreateProject({serverside = true, manual_files = false})
		kind "SharedLib"
		symbols "On"
		-- Remove some or all of these includes if they're not needed
		IncludeHelpersExtended()
		--IncludeLuaShared()
		IncludeSDKCommon()
		IncludeSDKTier0()
		IncludeSDKTier1()
		--IncludeSDKTier2()
		IncludeSDKTier3()
		IncludeSDKMathlib()
		--IncludeSDKRaytrace()
		--IncludeSDKBitmap()
		--IncludeSDKVTF()
		--IncludeSteamAPI()
		IncludeDetouring()
		IncludeScanning()

		files({
			[[source/modules/*.h]],
			[[source/modules/*.cpp]],
			[[source/sourcesdk/*.h]],
			[[source/sourcesdk/*.cpp]],
		})

		includedirs({
			[[Bootil/include/]]
		})

		libdirs(current_dir .. "/libs")
		filter("system:windows")
			files({"source/win32/*.cpp", "source/win32/*.hpp"})

		filter("system:windows", "platforms:x86_64")
			links({"bootil_static_64.lib"})

		filter("system:windows", "platforms:x86")
			links({"bootil_static_32.lib"})

		filter({"system:linux", "platforms:x86_64"})
			links("bootil_static_64")

		filter({"system:linux", "platforms:x86"})
			links("bootil_static_32")

		filter("system:linux")
			targetextension(".so")
			links -- this fixes the undefined reference to `dlopen' errors.
				{
					"dl",
					"tier0",
					"pthread"
				}

		filter("system:linux or macosx")
			files({"source/posix/*.cpp", "source/posix/*.hpp"})