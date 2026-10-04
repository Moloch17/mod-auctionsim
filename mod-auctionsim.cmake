set(AUCTIONSIM_DAT "${CMAKE_CURRENT_SOURCE_DIR}/mod-auctionsim/data/auctionsim.dat")
# Market mode's tables (ml/s6_export.py). Optional: Replay mode runs without it.
set(AUCTIONSIM_MARKET_DAT "${CMAKE_CURRENT_SOURCE_DIR}/mod-auctionsim/data/auctionsim_market.dat")

if(WIN32)
  add_custom_command(TARGET modules POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_FILE_DIR:worldserver>/configs/modules"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${AUCTIONSIM_DAT}"
      "$<TARGET_FILE_DIR:worldserver>/configs/modules/auctionsim.dat"
    COMMENT "Copy auctionsim.dat into configs/modules")
  install(FILES "${AUCTIONSIM_DAT}" DESTINATION "${CMAKE_INSTALL_PREFIX}/configs/modules")
  if(EXISTS "${AUCTIONSIM_MARKET_DAT}")
    add_custom_command(TARGET modules POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${AUCTIONSIM_MARKET_DAT}"
        "$<TARGET_FILE_DIR:worldserver>/configs/modules/auctionsim_market.dat"
      COMMENT "Copy auctionsim_market.dat into configs/modules")
  endif()
  install(FILES "${AUCTIONSIM_MARKET_DAT}" DESTINATION "${CMAKE_INSTALL_PREFIX}/configs/modules" OPTIONAL)
else()
  install(FILES "${AUCTIONSIM_DAT}" DESTINATION "${CONF_DIR}/modules")
  install(FILES "${AUCTIONSIM_MARKET_DAT}" DESTINATION "${CONF_DIR}/modules" OPTIONAL)
endif()

add_compile_options(-Wall -Wextra -Werror)
