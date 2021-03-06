cmake_minimum_required(VERSION 2.6)

project(BoxBackup_Windows)

set(boxbackup_dir ${CMAKE_SOURCE_DIR}/../../..)
set_property(DIRECTORY PROPERTY EP_BASE .)
set(install_dir ${CMAKE_BINARY_DIR}/install)

# Automate the process of downloading, building and "installing" dependencies on Windows,
# as used by AppVeyor.

# Version of zlib to download, build, and compile Box Backup against:
set(ZLIB_VERSION    1.2.11)
# Hash of zlib-${ZLIB_VERSION}.tar.gz, to be verified after download:
set(ZLIB_HASH SHA256=c3e5e9fdd5004dcb542feda5ee4f0ff0744628baf8ed2dd5d66f8ca1197cb1a1)

# Version of OpenSSL to download, build, and compile Box Backup against:
set(OPENSSL_VERSION 1.1.0g)
# Hash of openssl-${OPENSSL_VERSION}.tar.gz, to be verified after download:
set(OPENSSL_HASH SHA256=de4d501267da39310905cb6dc8c6121f7a2cad45a7707f76df828fe1b85073af)

# Version of PCRE to download, build, and compile Box Backup against:
set(PCRE_VERSION    8.39)
# Hash of pcre-${PCRE_VERSION}.tar.gz, to be verified after download:
set(PCRE_HASH SHA256=ccdf7e788769838f8285b3ee672ed573358202305ee361cfec7a4a4fb005bbc7)

# Version of Boost to download, unpack, and compile Box Backup against:
set(BOOST_VERSION    1.62.0)
# Hash of the Boost download file, to be verified after download:
set(BOOST_HASH SHA1=5fd97433c3f859d8cbab1eaed4156d3068ae3648)

include(ExternalProject)

ExternalProject_Add(zlib
	URL "http://zlib.net/zlib-${ZLIB_VERSION}.tar.gz"
	URL_HASH ${ZLIB_HASH}
	DOWNLOAD_NO_PROGRESS 1
	CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${install_dir} ${SUB_CMAKE_EXTRA_ARGS}
	# We need to build both versions, debug and release, because cmake requires both to be
	# present to generate its multi-configuration project files for Visual Studio/MSBuild.
	INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install --config Debug
	COMMAND         ${CMAKE_COMMAND} --build <BINARY_DIR> --target install --config Release
	STEP_TARGETS configure install
)

if(WIN32)
	ExternalProject_Add(openssl
		DEPENDS zlib
		URL "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
		URL_HASH ${OPENSSL_HASH}
		DOWNLOAD_NO_PROGRESS 1
		CONFIGURE_COMMAND perl Configure debug-VC-WIN32 no-asm no-shared
			--prefix=${install_dir}
			--openssldir=etc
		# Run tests before install, but don't make the main target depend on them, so that
		# we don't have to run them whenever we build manually on Windows.
		TEST_BEFORE_INSTALL 1
		TEST_EXCLUDE_FROM_MAIN 1
		# You would expect us to use nt.mak to compile a static library here, but mk1mf.pl uses the /MT[d]
		# CRT in that case, which is incompatible with our dynamic runtime, /MD[d]. It seems that the libs
		# built by ntdll.mak, which are compiled with /MD[d], are full libraries and not import libs,
		# so we can link statically against them and still get a dynamic runtime.
		BUILD_IN_SOURCE 1
		BUILD_COMMAND   nmake /s
		TEST_COMMAND    nmake /s test
		INSTALL_COMMAND nmake /s install
	)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	ExternalProject_Add(openssl
		DEPENDS zlib
		URL "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
		URL_HASH ${OPENSSL_HASH}
		DOWNLOAD_NO_PROGRESS 1
		CONFIGURE_COMMAND perl Configure darwin64-x86_64-cc --prefix=${install_dir}
		BUILD_IN_SOURCE 1
	)
else()
	ExternalProject_Add(openssl
		DEPENDS zlib
		URL "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"
		URL_HASH ${OPENSSL_HASH}
		DOWNLOAD_NO_PROGRESS 1
		CONFIGURE_COMMAND ./config --prefix=${install_dir}
		BUILD_IN_SOURCE 1
	)

endif()

ExternalProject_Add(pcre
	# Temporarily use SVN repo until the PCRE_STATIC issue in 8.40 is fixed:
	# https://vcs.pcre.org/pcre?view=revision&revision=1677
	SVN_REPOSITORY svn://vcs.exim.org/pcre/code/trunk
	SVN_REVISION -r 1677
	DOWNLOAD_NO_PROGRESS 1
	CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${install_dir} ${SUB_CMAKE_EXTRA_ARGS}
	-DPCRE_SUPPORT_LIBREADLINE=OFF
	-DPCRE_SUPPORT_LIBBZ2=OFF
	# We need to build both versions, debug and release, because cmake requires both to be
	# present to generate its multi-configuration project files for Visual Studio/MSBuild.
	INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install --config Debug
	COMMAND         ${CMAKE_COMMAND} --build <BINARY_DIR> --target install --config Release
)

string(REPLACE "." "_" BOOST_VERSION_UNDERSCORES ${BOOST_VERSION})
ExternalProject_Add(boost
	URL "http://downloads.sourceforge.net/project/boost/boost/${BOOST_VERSION}/boost_${BOOST_VERSION_UNDERSCORES}.tar.bz2"
	URL_HASH ${BOOST_HASH}
	# DOWNLOAD_NO_PROGRESS 1
	# Disable automatic updating (untarring) as it's slow and not necessary
	UPDATE_DISCONNECTED 1
	CONFIGURE_COMMAND ${CMAKE_COMMAND} -E echo "No configure step needed"
	BUILD_COMMAND ${CMAKE_COMMAND} -E echo "No build step needed"
	INSTALL_COMMAND ${CMAKE_COMMAND} -E echo "No install step needed"
)

if(BOXBACKUP_VERSION)
	list(APPEND boxbackup_cmake_args "-DBOXBACKUP_VERSION=${BOXBACKUP_VERSION}")
endif()
string(REPLACE ";" " " boxbackup_cmake_args "${boxbackup_cmake_args}")

ExternalProject_Add(boxbackup
	DEPENDS zlib openssl pcre boost
	SOURCE_DIR ${boxbackup_dir}/infrastructure/cmake
	CMAKE_ARGS
		-DZLIB_ROOT=${install_dir}
		-DOPENSSL_ROOT_DIR=${install_dir}
		-DPCRE_ROOT=${install_dir}
		-DBOOST_ROOT=${CMAKE_BINARY_DIR}/Source/boost
		-DBOX_SUPPORT_READLINE=OFF
		-DCMAKE_INSTALL_PREFIX=${install_dir}
		-DAPPVEYOR_MODE=1
		-DDEBUG=${DEBUG}
		${boxbackup_cmake_args}
		${SUB_CMAKE_EXTRA_ARGS}
	INSTALL_COMMAND ${CMAKE_COMMAND} -E echo "No install step needed"
	STEP_TARGETS configure build
)
