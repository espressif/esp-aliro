idf_component_register(SRC_DIRS         "."
                       INCLUDE_DIRS     ".")

target_add_binary_data(${COMPONENT_TARGET} "certs/reader_pubkey.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/reader_privkey.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/reader_cert.der" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/credential_pubkey.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/reader_cert_issuer_ca_pubkey.pem" TEXT)
