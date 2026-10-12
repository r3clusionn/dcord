# Vendored DAVE (Discord voice E2EE) stack: Cisco mlspp + Discord libdave,
# both built as static libraries against the system OpenSSL 3.
crypto_dep = dependency('libcrypto')
tp_args = ['-DWITH_OPENSSL3', '-DDISABLE_GREASE', '-w']

mlspp_inc = include_directories(
  'mlspp/include', 'mlspp/lib/bytes/include', 'mlspp/lib/hpke/include',
  'mlspp/lib/tls_syntax/include', 'mlspp/third_party', 'nlohmann', '.')

mlspp_lib = static_library('mlspp',
  files(
    'mlspp/src/common.cpp', 'mlspp/src/core_types.cpp', 'mlspp/src/credential.cpp',
    'mlspp/src/crypto.cpp', 'mlspp/src/grease.cpp', 'mlspp/src/key_schedule.cpp',
    'mlspp/src/messages.cpp', 'mlspp/src/session.cpp', 'mlspp/src/state.cpp',
    'mlspp/src/treekem.cpp', 'mlspp/src/tree_math.cpp',
    'mlspp/lib/bytes/src/bytes.cpp', 'mlspp/lib/tls_syntax/src/tls_syntax.cpp',
    'mlspp/lib/hpke/src/aead_cipher.cpp', 'mlspp/lib/hpke/src/base64.cpp',
    'mlspp/lib/hpke/src/certificate.cpp', 'mlspp/lib/hpke/src/common.cpp',
    'mlspp/lib/hpke/src/dhkem.cpp', 'mlspp/lib/hpke/src/digest.cpp',
    'mlspp/lib/hpke/src/group.cpp', 'mlspp/lib/hpke/src/hkdf.cpp',
    'mlspp/lib/hpke/src/hpke.cpp', 'mlspp/lib/hpke/src/openssl_common.cpp',
    'mlspp/lib/hpke/src/random.cpp', 'mlspp/lib/hpke/src/rsa.cpp',
    'mlspp/lib/hpke/src/signature.cpp', 'mlspp/lib/hpke/src/userinfo_vc.cpp'),
  include_directories: mlspp_inc,
  cpp_args: tp_args,
  dependencies: crypto_dep)

dave_lib = static_library('dave',
  files(
    'libdave/src/bindings_capi.cpp', 'libdave/src/codec_utils.cpp',
    'libdave/src/cryptor.cpp', 'libdave/src/cryptor_manager.cpp',
    'libdave/src/decryptor.cpp', 'libdave/src/encryptor.cpp',
    'libdave/src/frame_processors.cpp', 'libdave/src/logger.cpp',
    'libdave/src/mls_key_ratchet.cpp', 'libdave/src/openssl_cryptor.cpp',
    'libdave/src/version.cpp', 'libdave/src/utils/leb128.cpp',
    'libdave/src/mls/key_serialization.cpp', 'libdave/src/mls/parameters.cpp',
    'libdave/src/mls/persisted_key_pair_null.cpp', 'libdave/src/mls/session.cpp',
    'libdave/src/mls/user_credential.cpp', 'libdave/src/mls/util.cpp'),
  include_directories: [mlspp_inc, include_directories('libdave/includes', 'libdave/src')],
  cpp_args: tp_args,
  dependencies: crypto_dep)

dave_dep = declare_dependency(
  include_directories: include_directories('libdave/includes'),
  link_with: [dave_lib, mlspp_lib],
  dependencies: crypto_dep)

qrcodegen_dep = declare_dependency(
  include_directories: include_directories('qrcodegen'),
  sources: files('qrcodegen/qrcodegen.c'))

json_inc = include_directories('nlohmann')
