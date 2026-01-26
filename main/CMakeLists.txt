idf_component_register(SRCS "main.c" "dns_server.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES
                        "certs/device_cert.pem.crt"
                        "certs/private_key.pem.key"
                        "certs/amazon_root_ca.pem")
