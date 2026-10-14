set(sources app_main.c fixture.c)
if(CONFIG_SUPPORT_FAST_TRANSACTION_PHASE)
    list(APPEND sources cache.c)
endif()
if(CONFIG_SUPPORT_STEPUP_PHASE)
    list(APPEND sources access_document.c)
endif()

idf_component_register(SRCS ${sources}
                       INCLUDE_DIRS ".")

target_add_binary_data(${COMPONENT_TARGET} "certs/reader_pubkey.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/reader_privkey.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/reader_cert.der" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/credential_pubkey.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/document_issuer_pubkey.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/reader_cert_issuer_ca_pubkey.pem" TEXT)
