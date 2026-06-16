project (mbedtls
  LANGUAGES C CXX
)

# (TAO) This whole implementation is used to remove dependency on Windows SSL implementation and improve portability
#   It's not portable but also not used much in the engine, so use an embedded TLS implementation that is well maintained and small

if (NOT EXISTS "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}")
  message("-- Downloading mbedtls")
  file(DOWNLOAD "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}.tar.bz2"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}.tar.bz2"
    SHOW_PROGRESS
  )
  file(ARCHIVE_EXTRACT
    INPUT "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}.tar.bz2"
    DESTINATION "${SDK_DIR}"
  )
  file(REMOVE "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}.tar.bz2")
  message("-- Downloaded mbedtls")
  execute_process (COMMAND cd "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}")
  execute_process (COMMAND git submodule init)
  execute_process (COMMAND git submodule update)
  execute_process (COMMAND cd "${CMAKE_CURRENT_SOURCE_DIR}")
else()
  message("-- Mbedtls version ${MBEDTLS_VERSION} found")
endif()

#--- Python3 is required for generating needed files ---#
find_package (Python3 COMPONENTS Interpreter REQUIRED)

#--- Set up a local VENV to store required modules in without polluting the host system ---#
#--- Uses recommended implementation from: https://discourse.cmake.org/t/possible-to-create-a-python-virtual-env-from-cmake-and-then-find-it-with-findpython3/1132/2 ---#
set(VENV_DIR "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/venv")
execute_process (COMMAND "${Python3_EXECUTABLE}" -m venv "${VENV_DIR}")
set (ENV{VIRTUAL_ENV} "${VENV_DIR}")
set (ENV{MBEDTLS_DIR} "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}")
set (Python3_FIND_VIRTUALENV FIRST)
unset (Python3_EXECUTABLE)
find_package (Python3 COMPONENTS Interpreter Development REQUIRED)

#--- Install the required libraries in the VENV ---#
execute_process (COMMAND "${Python3_EXECUTABLE}" -m pip install -r "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/scripts/basic.requirements.txt")

#--- Run a required Windows step ---#
if (Windows)
  execute_process (COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/make_generated_files.py"
  )
endif()

# Added from original CMake file because these are required
find_package(Perl REQUIRED)

# Wrap these so I don't have to rewrite most of this
set (MBEDTLS_DIR ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION})
set (MBEDTLS_FRAMEWORK_DIR ${MBEDTLS_DIR}/framework)
set (MBEDTLS_PYTHON_EXECUTABLE ${Python3_EXECUTABLE})
set (TF_PSA_CRYPTO_DRIVERS_BUILTIN_SRC_DIR "${MBEDTLS_DIR}/tf-psa-crypto/drivers/builtin/src")
set (ERROR_C "${MBEDTLS_DIR}/library/error.c") # Doesn't generate correctly when in ${CMAKE_CURRENT_BINARY_DIR}, so workaround here; DO NOT change

file(GLOB crypto_error_headers ${MBEDTLS_DIR}/library/include/mbedtls/*.h)
file(GLOB tls_error_headers ${MBEDTLS_DIR}/include/mbedtls/*.h)
add_custom_command(
    OUTPUT ${ERROR_C}
    COMMAND
        ${PERL_EXECUTABLE}
            ${MBEDTLS_DIR}/scripts/generate_errors.pl
            ${MBEDTLS_DIR}/tf-psa-crypto/drivers/builtin/include/mbedtls
            ${MBEDTLS_DIR}/include/mbedtls
            ${MBEDTLS_DIR}/scripts/data_files
            ${TF_PSA_CRYPTO_DRIVERS_BUILTIN_SRC_DIR}/error.c
    DEPENDS
        ${MBEDTLS_DIR}/scripts/generate_errors.pl
        ${crypto_error_headers}
        ${tls_error_headers}
        ${MBEDTLS_DIR}/scripts/data_files/error.fmt
)

add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/version_features.c
    COMMAND
        ${PERL_EXECUTABLE}
            ${MBEDTLS_DIR}/scripts/generate_features.pl
            ${MBEDTLS_DIR}/include/mbedtls
            ${MBEDTLS_DIR}/scripts/data_files
            ${CMAKE_CURRENT_BINARY_DIR}/version_features.c
    DEPENDS
        ${MBEDTLS_DIR}/scripts/generate_features.pl
        ${MBEDTLS_DIR}/include/mbedtls/mbedtls_config.h
        ${MBEDTLS_DIR}/scripts/data_files/version_features.fmt
)

execute_process(
    COMMAND
        ${MBEDTLS_PYTHON_EXECUTABLE}
        ${MBEDTLS_DIR}/scripts/generate_config_checks.py
        --list-for-cmake "${CMAKE_CURRENT_BINARY_DIR}"
    WORKING_DIRECTORY
        ${MBEDTLS_DIR}
    OUTPUT_VARIABLE
        MBEDTLS_GENERATED_CONFIG_CHECKS_HEADERS
)

add_custom_command(
    OUTPUT ${MBEDTLS_GENERATED_CONFIG_CHECKS_HEADERS}
    COMMAND
        ${MBEDTLS_PYTHON_EXECUTABLE}
            ${MBEDTLS_DIR}/scripts/generate_config_checks.py
            ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS
        ${MBEDTLS_DIR}/scripts/generate_config_checks.py
        ${MBEDTLS_FRAMEWORK_DIR}/scripts/mbedtls_framework/config_checks_generator.py
)

add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/ssl_debug_helpers_generated.c
    COMMAND
        ${MBEDTLS_PYTHON_EXECUTABLE}
            ${MBEDTLS_DIR}/framework/scripts/generate_ssl_debug_helpers.py
            --mbedtls-root ${MBEDTLS_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS
        ${MBEDTLS_DIR}/framework/scripts/generate_ssl_debug_helpers.py
        ${tls_error_headers}
)

add_custom_target(${MBEDTLS_TARGET_PREFIX}libmbedx509_generated_files_target
    DEPENDS
      ${ERROR_C}
      ${MBEDTLS_GENERATED_CONFIG_CHECKS_HEADERS}
)

add_custom_target(${MBEDTLS_TARGET_PREFIX}libmbedtls_generated_files_target
    DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/ssl_debug_helpers_generated.c
        ${CMAKE_CURRENT_BINARY_DIR}/version_features.c
)

# List generated headers as sources explicitly. Normally CMake finds
# headers by tracing include directives, but if that happens before the
# generated headers are generated, this process doesn't find them.
list(APPEND src_x509
    ${MBEDTLS_GENERATED_CONFIG_CHECKS_HEADERS}
)

#--- Build the library ---#

set (SOURCE_LIBRARY
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/debug.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/mbedtls_config.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/mps_reader.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/mps_trace.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/net_sockets.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/pkcs7.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_cache.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_ciphersuites.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_client.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_cookie.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_msg.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_ticket.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_tls.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_tls12_client.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_tls12_server.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_tls13_client.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_tls13_generic.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_tls13_keys.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/ssl_tls13_server.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/timing.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/version.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/x509.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/x509_create.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/x509_crl.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/x509_crt.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/x509_csr.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/x509_oid.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/x509write.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/x509write_crt.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library/x509write_csr.c"
)
source_group ("library" FILES ${SOURCE_LIBRARY})

set (SOURCE_TF_PSA_CRYPTO_CORE
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/core/psa_crypto_client.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/core/psa_crypto_driver_wrappers_no_static.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/core/psa_crypto_slot_management.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/core/psa_crypto_storage.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/core/psa_crypto.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/core/psa_its_file.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/core/tf_psa_crypto_config.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/core/tf_psa_crypto_version.c"
)
source_group ("tf-psa-crypto\\core" FILES ${SOURCE_TF_PSA_CRYPTO_CORE})

# Only use the built in drivers for now; Everest and P256-a are external projects that need extra dependencies to work
set (SOURCE_TF_PSA_CRYPTO_DRIVERS
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/aes.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/aesce.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/aesni.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/aria.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/asn1parse.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/asn1write.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/base64.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/bignum_core.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/bignum_mod_raw.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/bignum_mod.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/bignum.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/block_cipher.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/camellia.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/ccm.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/chacha20.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/chachapoly.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/cipher_wrap.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/cipher.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/cmac.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/constant_time.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/ctr_drbg.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/ecdh.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/ecdsa.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/ecjpake.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/ecp_curves_new.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/ecp_curves.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/ecp.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/entropy_poll.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/entropy.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/gcm.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/hmac_drbg.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/lmots.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/lms.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/md.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/md5.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/memory_buffer_alloc.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/nist_kw.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/oid.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/pem.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/pk_ecc.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/pk_rsa.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/pk_wrap.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/pk.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/pkcs5.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/pkparse.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/pkwrite.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/platform_util.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/platform.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/poly1305.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/psa_crypto_aead.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/psa_crypto_cipher.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/psa_crypto_ecp.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/psa_crypto_ffdh.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/psa_crypto_hash.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/psa_crypto_mac.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/psa_crypto_pake.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/psa_crypto_rsa.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/psa_util.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/ripemd160.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/rsa_alt_helpers.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/rsa.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/sha1.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/sha256.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/sha3.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/sha512.c"
    "${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src/threading.c"
)
source_group ("tf-psa-crypto\\drivers" FILES ${SOURCE_TF_PSA_CRYPTO_DRIVERS})

set (SOURCE_GEN
  "${ERROR_C}"
  "${CMAKE_CURRENT_BINARY_DIR}/ssl_debug_helpers_generated.c"
  "${CMAKE_CURRENT_BINARY_DIR}/version_features.c"
)
source_group ("gen" FILES ${SOURCE_GEN})

if (COMMAND CryEngineStaticModule)
  set(${PROJECT_NAME}_SOURCES ${SOURCE_LIBRARY}
    ${SOURCE_TF_PSA_CRYPTO_CORE}
    ${SOURCE_TF_PSA_CRYPTO_DRIVERS}
    ${SOURCE_GEN}
  )
	CryEngineStaticModule(${PROJECT_NAME} SOLUTION_FOLDER "Libs")
else()
	add_library (${PROJECT_NAME} STATIC ${SOURCE_LIBRARY}
    ${SOURCE_TF_PSA_CRYPTO_CORE}
    ${SOURCE_TF_PSA_CRYPTO_DRIVERS}
    ${SOURCE_GEN}
  )
	set_solution_folder("Libs" ${PROJECT_NAME})
endif()

target_include_directories (${PROJECT_NAME}
  PUBLIC ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/include
  PUBLIC ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/include
  PRIVATE ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/library
  PRIVATE ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/core
  PRIVATE ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/src
  PRIVATE ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/include
  PRIVATE ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/src
)
