file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/mod-auctionsim/data/auctionsim.dat
     DESTINATION ${CMAKE_SOURCE_DIR}/env/dist/etc/modules)

add_compile_options(-Wall -Wextra -Werror)
