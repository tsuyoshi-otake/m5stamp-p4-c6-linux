# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
idf_component_register(SRCS "main.c")
ulp_embed_binary(ulp_main "ulp/main.c" "main.c")
