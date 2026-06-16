project (curl C)

# (TAO) Embedding mbedtls (an embedded Openssl implementation) here since it's a build dependency of newer versions of Curl
#   The Windows SSL implementation originally used was both not portable to other platforms and subject to breakage
set (MBEDTLS_VERSION "")
if (NOT MBEDTLS_VERSION)
  set (MBEDTLS_VERSION "4.0.0") # Latest stable and recommended release at time of implementation
endif()

add_subdirectory(mbedtls)

#--- Grab curl source code ---#
set (CURL_VERSION "")
set (ZIP_VERSION "")

if (NOT CURL_VERSION)
  set (CURL_VERSION "8_17_0") # Latest stable and recommended release at time of implementation
endif()

string(REPLACE "_" "." ZIP_VERSION ${CURL_VERSION}) # Curl's official release tags and versioning scheme are inconsistent, so fix that here

if (NOT EXISTS "${SDK_DIR}/curl-${ZIP_VERSION}")
  message("-- Downloading curl")
  file(DOWNLOAD "https://github.com/curl/curl/releases/download/curl-${CURL_VERSION}/curl-${ZIP_VERSION}.zip"
    "${SDK_DIR}/curl-${ZIP_VERSION}.zip"
    SHOW_PROGRESS
  )
  file(ARCHIVE_EXTRACT
    INPUT "${SDK_DIR}/curl-${ZIP_VERSION}.zip"
    DESTINATION "${SDK_DIR}"
  )
  file(REMOVE "${SDK_DIR}/curl-${ZIP_VERSION}.zip")
  message("-- Downloaded curl")
else()
  message("-- Curl version ${ZIP_VERSION} found")
endif()

set(CURL_VERSION "${ZIP_VERSION}")

#---

set (SOURCE_ROOT
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/altsvc.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/amigaos.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/asyn-ares.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/asyn-base.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/asyn-thrdd.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/bufq.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/bufref.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/cf-h1-proxy.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/cf-h2-proxy.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/cf-haproxy.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/cf-https-connect.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/cfilters.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/cf-ip-happy.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/cf-socket.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/conncache.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/connect.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/content_encoding.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/cookie.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/cshutdn.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_addrinfo.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_endian.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_fnmatch.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_fopen.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_get_line.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_gethostname.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_gssapi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_memrchr.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_ntlm_core.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_range.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_rtmp.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_sasl.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_sha512_256.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_sspi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_threads.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/curl_trc.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/dict.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/dllmain.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/doh.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/dynhds.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/easy.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/easygetopt.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/easyoptions.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/escape.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/fake_addrinfo.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/file.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/fileinfo.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/formdata.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/ftp.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/ftplistparser.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/getenv.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/getinfo.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/gopher.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/hash.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/headers.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/hmac.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/hostip.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/hostip4.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/hostip6.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/hsts.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/http.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/http1.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/http2.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/http_chunks.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/http_digest.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/http_negotiate.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/http_ntlm.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/http_proxy.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/httpsrr.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/idn.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/if2ip.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/imap.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/ldap.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/llist.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/macos.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/md4.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/md5.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/memdebug.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/mime.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/mprintf.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/mqtt.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/multi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/multi_ev.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/multi_ntfy.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/netrc.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/noproxy.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/openldap.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/parsedate.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/pingpong.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/pop3.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/progress.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/psl.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/rand.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/rename.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/request.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/rtsp.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/select.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/sendf.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/setopt.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/sha256.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/share.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/slist.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/smb.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/smtp.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/socketpair.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/socks.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/socks_gssapi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/socks_sspi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/speedcheck.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/splay.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/strcase.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/strdup.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/strequal.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/strerror.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/system_win32.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/telnet.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/tftp.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/transfer.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/uint-bset.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/uint-hash.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/uint-spbset.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/uint-table.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/url.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/urlapi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/version.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/ws.c"
)
source_group ("lib" FILES ${SOURCE_ROOT})

# These two files exist in the source tree, but CMake parses their names wrong somehow;
#   DO NOT change to ${SDK_DIR}; original code had it this way for similar files with this issue. Will break!!!
set (SOURCE_CW
  "../../SDKs/curl-${CURL_VERSION}/lib/cw-out.c"
  "../../SDKs/curl-${CURL_VERSION}/lib/cw-pause.c"
)
source_group ("cw" FILES ${SOURCE_CW})

set (SOURCE_CURLX
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/base64.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/dynbuf.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/fopen.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/inet_ntop.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/inet_pton.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/multibyte.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/nonblock.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/strerr.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/strparse.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/timediff.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/timeval.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/version_win32.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/wait.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/warnless.c"
  "${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx/winapi.c"
)
source_group ("lib\\curlx" FILES ${SOURCE_CURLX})

set (SOURCE_VAUTH
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/cleartext.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/cram.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/digest.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/digest_sspi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/gsasl.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/krb5_gssapi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/krb5_sspi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/ntlm.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/ntlm_sspi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/oauth2.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/spnego_gssapi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/spnego_sspi.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth/vauth.c"
)
source_group ("lib\\vauth" FILES ${SOURCE_VAUTH})

set (SOURCE_VQUIC
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vquic/curl_ngtcp2.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vquic/curl_osslq.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vquic/curl_quiche.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vquic/vquic.c"
)
source_group ("lib\\quic" FILES ${SOURCE_VQUIC})

set (SOURCE_VSSH
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vssh/curl_path.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vssh/libssh.c"
)
source_group ("lib\\vssh" FILES ${SOURCE_VSSH})

set (SOURCE_VTLS
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/apple.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/cipher_suite.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/gtls.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/hostcheck.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/keylog.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/mbedtls.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/mbedtls_threadlock.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/openssl.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/rustls.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/schannel.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/schannel_verify.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/vtls.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/vtls_scache.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/vtls_spack.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/wolfssl.c"
	"${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls/x509asn1.c"
)
source_group ("lib\\vtls" FILES ${SOURCE_VTLS})

#---

if (COMMAND CryEngineStaticModule)
  set(${PROJECT_NAME}_SOURCES ${SOURCE_ROOT}
    ${SOURCE_CW}
    ${SOURCE_CURLX}
    ${SOURCE_VQUIC}
    ${SOURCE_VSSH}
    ${SOURCE_VAUTH}
    ${SOURCE_VTLS}
  )
	CryEngineStaticModule(${PROJECT_NAME} SOLUTION_FOLDER "Libs")
else()
	add_library (${PROJECT_NAME} STATIC ${SOURCE_ROOT}
    ${SOURCE_CW}
    ${SOURCE_CURLX}
    ${SOURCE_VQUIC}
    ${SOURCE_VSSH}
    ${SOURCE_VAUTH}
    ${SOURCE_VTLS}
  )
	set_solution_folder("Libs" ${PROJECT_NAME})
endif()

target_include_directories (${PROJECT_NAME}
	PUBLIC ${SDK_DIR}/curl-${CURL_VERSION}/include

# Curl's USE_MBEDTLS requires headers from these directories to explicitly get called
  PRIVATE ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/include
  PRIVATE ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/include
  PRIVATE ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin
  PRIVATE ${SDK_DIR}/mbedtls-${MBEDTLS_VERSION}/tf-psa-crypto/drivers/builtin/include

# The rest of the Curl headers
  PRIVATE ${SDK_DIR}/curl-${CURL_VERSION}/lib
	PRIVATE ${SDK_DIR}/curl-${CURL_VERSION}/lib/curlx
	PRIVATE ${SDK_DIR}/curl-${CURL_VERSION}/lib/vauth
	PRIVATE ${SDK_DIR}/curl-${CURL_VERSION}/lib/vquic
	PRIVATE ${SDK_DIR}/curl-${CURL_VERSION}/lib/vssh
	PRIVATE ${SDK_DIR}/curl-${CURL_VERSION}/lib/vtls
)

target_compile_definitions(${PROJECT_NAME}
  PUBLIC CURL_STATICLIB

	PRIVATE
	BUILDING_LIBCURL

	# disable unused features
	#CURL_DISABLE_CRYPTO_AUTH
	CURL_DISABLE_PROXY
	CURL_DISABLE_COOKIES

	# disable all unused protocols
	#CURL_DISABLE_HTTP
  CURL_DISABLE_AWS # This may need to be reenabled if AWS server support (used in many games) becomes requested; expect breakage at first
	CURL_DISABLE_DICT
	CURL_DISABLE_FILE
	CURL_DISABLE_FTP
	CURL_DISABLE_GOPHER
	CURL_DISABLE_IMAP
	CURL_DISABLE_TFTP
	CURL_DISABLE_LDAP
	CURL_DISABLE_POP3
	CURL_DISABLE_RTSP
	CURL_DISABLE_SMB
	CURL_DISABLE_SMTP
	CURL_DISABLE_TELNET
)

#--- available modules

target_compile_definitions(${PROJECT_NAME} PRIVATE
	HAVE_ZLIB_H
	HAVE_LIBZ
)
target_link_libraries (${PROJECT_NAME} PRIVATE zlib)

#--- other

target_compile_definitions(${PROJECT_NAME} PRIVATE
  USE_MBEDTLS # Replaced Windows native SSL with something more portable
)
target_link_libraries (${PROJECT_NAME} INTERFACE mbedtls)

#--- configure ---
#--- Should use include() to divide configure settings to a seperate file

# Above code hasn't been tested with Linux or Mac; could break next section of code if these values were changed.
#   Also, curl_config.h.in will requiring editing too  in that case
if (NOT WINDOWS)
# Generate custom configuration file

#--- HEADER FILES
include (CheckIncludeFiles)

check_include_files ("arpa/inet.h" HAVE_ARPA_INET_H)
check_include_files ("assert.h" HAVE_ASSERT_H)
check_include_files ("crypto.h" HAVE_CRYPTO_H)
check_include_files ("errno.h" HAVE_ERRNO_H)
check_include_files ("err.h" HAVE_ERR_H)
check_include_files ("fcntl.h" HAVE_FCNTL_H)
check_include_files ("getopt.h" HAVE_GETOPT_H)
check_include_files ("inttypes.h" HAVE_INTTYPES_H)
check_include_files ("io.h" HAVE_IO_H)
check_include_files ("limits.h" HAVE_LIMITS_H)
check_include_files ("locale.h" HAVE_LOCALE_H)
check_include_files ("netdb.h" HAVE_NETDB_H)
check_include_files ("netinet/in.h" HAVE_NETINET_IN_H)
check_include_files ("process.h" HAVE_PROCESS_H)
check_include_files ("pthread.h" HAVE_PTHREAD_H)
check_include_files ("signal.h" HAVE_SIGNAL_H)
check_include_files ("sgtty.h" HAVE_SGTTY_H)
check_include_files ("ssl.h" HAVE_SSL_H)
check_include_files ("stdbool.h" HAVE_STDBOOL_H)
check_include_files ("stdlib.h" HAVE_STDLIB_H)
check_include_files ("sys/param.h" HAVE_SYS_PARAM_H)
check_include_files ("sys/select.h" HAVE_SYS_SELECT_H)
check_include_files ("sys/socket.h" HAVE_SYS_SOCKET_H)
check_include_files ("sys/sockio.h" HAVE_SYS_SOCKIO_H)
check_include_files ("sys/stat.h" HAVE_SYS_STAT_H)
check_include_files ("sys/time.h" HAVE_SYS_TIME_H)
check_include_files ("sys/types.h" HAVE_SYS_TYPES_H)
check_include_files ("sys/utime.h" HAVE_SYS_UTIME_H)
check_include_files ("termio.h" HAVE_TERMIO_H)
check_include_files ("termios.h" HAVE_TERMIOS_H)
check_include_files ("time.h" HAVE_TIME_H)
check_include_files ("unistd.h" HAVE_UNISTD_H)
check_include_files ("windows.h" HAVE_WINDOWS_H)
check_include_files ("winsock.h" HAVE_WINSOCK_H)
check_include_files ("winsock2.h" HAVE_WINSOCK2_H)
check_include_files ("ws2tcpip.h" HAVE_WS2TCPIP_H)

#--- FUNCTIONS
set (CURL_INCLUDES
	"string.h"
	"unistd.h"
	"netdb.h"
	"sys/time.h"
	"arpa/inet.h"
	"stdio.h"
	"sys/select.h"
	"locale.h"
	"sys/socket.h"
	"time.h"
	"stdlib.h"
	"termios.h"
	"utime.h"
)

include (CheckSymbolExists)
include (CheckFunctionExists)

check_symbol_exists(closesocket   "${CURL_INCLUDES}" HAVE_CLOSESOCKET)
#define HAVE_DOPRNT 1
check_symbol_exists(ftruncate      "${CURL_INCLUDES}" HAVE_FTRUNCATE)
check_symbol_exists(gethostbyaddr "${CURL_INCLUDES}" HAVE_GETHOSTBYADDR)
check_function_exists(gethostname HAVE_GETHOSTNAME)
#define HAVE_GETPASS 1
#define HAVE_GETSERVBYNAME 1
check_symbol_exists(getprotobyname "${CURL_INCLUDES}" HAVE_GETPROTOBYNAME)
check_symbol_exists(gettimeofday  "${CURL_INCLUDES}" HAVE_GETTIMEOFDAY)
check_symbol_exists(inet_addr     "${CURL_INCLUDES}" HAVE_INET_ADDR)
#define HAVE_IOCTLSOCKET 1
#define HAVE_IOCTLSOCKET_FIONBIO 1
check_symbol_exists(perror        "${CURL_INCLUDES}" HAVE_PERROR)
check_symbol_exists(RAND_screen   "${CURL_INCLUDES}" HAVE_RAND_SCREEN)
check_symbol_exists(RAND_status   "${CURL_INCLUDES}" HAVE_RAND_STATUS)
check_symbol_exists(CRYPTO_cleanup_all_ex_data "${CURL_INCLUDES}"
    HAVE_CRYPTO_CLEANUP_ALL_EX_DATA)
check_symbol_exists(select        "${CURL_INCLUDES}" HAVE_SELECT)
check_symbol_exists(setlocale      "${CURL_INCLUDES}" HAVE_SETLOCALE)
#define HAVE_SETMODE 1
check_symbol_exists(setvbuf       "${CURL_INCLUDES}" HAVE_SETVBUF)
check_symbol_exists(socket        "${CURL_INCLUDES}" HAVE_SOCKET)
check_symbol_exists(strcasecmp    "${CURL_INCLUDES}" HAVE_STRCASECMP)
check_symbol_exists(strdup        "${CURL_INCLUDES}" HAVE_STRDUP)
check_symbol_exists(strftime      "${CURL_INCLUDES}" HAVE_STRFTIME)
check_symbol_exists(stricmp       "${CURL_INCLUDES}" HAVE_STRICMP)
#define HAVE_STRNCASECMP 1
#define HAVE_STRNICMP 1
check_symbol_exists(strstr        "${CURL_INCLUDES}" HAVE_STRSTR)
check_symbol_exists(strtoll        "${CURL_INCLUDES}" HAVE_STRTOLL)
check_symbol_exists(tcgetattr     "${CURL_INCLUDES}" HAVE_TCGETATTR)
check_symbol_exists(tcsetattr     "${CURL_INCLUDES}" HAVE_TCSETATTR)
check_symbol_exists(utime         "${CURL_INCLUDES}" HAVE_UTIME)

#define GETNAMEINFO_QUAL_ARG1 const
#define GETNAMEINFO_TYPE_ARG1 struct sockaddr *
#define GETNAMEINFO_TYPE_ARG2 socklen_t
#define GETNAMEINFO_TYPE_ARG46 DWORD
#define GETNAMEINFO_TYPE_ARG7 int

check_symbol_exists(recv "${CURL_INCLUDES}" HAVE_RECV)
#define RECV_TYPE_ARG1 SOCKET
#define RECV_TYPE_ARG2 char *
#define RECV_TYPE_ARG3 int
#define RECV_TYPE_ARG4 int
#define RECV_TYPE_RETV int

check_symbol_exists(recvfrom "${CURL_INCLUDES}" HAVE_RECVFROM)
#define RECVFROM_TYPE_ARG1 SOCKET
#define RECVFROM_TYPE_ARG2 char
#define RECVFROM_TYPE_ARG3 int
#define RECVFROM_TYPE_ARG4 int
#define RECVFROM_TYPE_ARG5 struct sockaddr
#define RECVFROM_TYPE_ARG6 int
#define RECVFROM_TYPE_RETV int

check_symbol_exists(send "${CURL_INCLUDES}" HAVE_SEND)
#define SEND_TYPE_ARG1 SOCKET
#define SEND_QUAL_ARG2 const
#define SEND_TYPE_ARG2 char *
#define SEND_TYPE_ARG3 int
#define SEND_TYPE_ARG4 int
#define SEND_TYPE_RETV int

#--- TYPE SIZES
include (CheckTypeSize)

check_type_size("int"  SIZEOF_INT)
check_type_size("long double"  SIZEOF_LONG_DOUBLE)
check_type_size("long long"  SIZEOF_LONG_LONG)
check_type_size("short"  SIZEOF_SHORT)
check_type_size("size_t" SIZEOF_SIZE_T)

#---- COMPILER SPECIFIC

if (HAVE_SIZEOF_LONG_LONG)
  set(HAVE_LONGLONG 1)
  set(HAVE_LL 1)
endif (HAVE_SIZEOF_LONG_LONG)

#--- STRUCT RELATED

set (CMAKE_EXTRA_INCLUDE_FILES ${CURL_INCLUDES})
check_type_size("struct timeval" STRUCT_TIMEVAL)
set (CMAKE_EXTRA_INCLUDE_FILES)

#--- ADDITIONAL DEFINITIONS

set (OS "\"${CMAKE_SYSTEM_NAME}\"")

#--- MORE

if(HAVE_PTHREAD_H)
	set(USE_THREADS_POSIX 1)
endif()

#---

configure_file ("${CMAKE_CURRENT_SOURCE_DIR}/curl_config.h.in" "${CMAKE_CURRENT_BINARY_DIR}/curl_config.h")
target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_CONFIG_H)
target_include_directories (${PROJECT_NAME} PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
)

endif (NOT WINDOWS)

if (ORBIS OR DARWIN OR LINUX OR ANDROID)
	target_compile_options(${THIS_PROJECT} PRIVATE -w)
endif()
