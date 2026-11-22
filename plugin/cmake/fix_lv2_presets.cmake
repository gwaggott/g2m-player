# Run after DPF writes presets.ttl (cmake -DFILE=<presets.ttl> -P ...):
# - DPF ends a preset's port list early ("] .") when an output parameter
#   follows an input, so the rest of the list isn't valid Turtle: join it up;
# - presets keep the current model and leave the Sustain switch alone
#   (factory_presets.h), so their Model and Sustain ports are dropped.
file(READ "${FILE}" ttl)
string(REPLACE "    ] .\n\n    [\n" "    ] ,\n    [\n" ttl "${ttl}")
foreach(symbol model_choice sustain)
  string(REGEX REPLACE "    \\[\n        lv2:symbol \"${symbol}\" ;\n        pset:value [^\n]* ;\n    \\] ,\n" "" ttl "${ttl}")
endforeach()
if(ttl MATCHES "lv2:symbol \"(model_choice|sustain)\"")
  message(FATAL_ERROR "fix_lv2_presets.cmake: ${CMAKE_MATCH_1} still in ${FILE}")
endif()
file(WRITE "${FILE}" "${ttl}")
