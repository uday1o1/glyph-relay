add_executable(glyphrelay_uniform_aq_evaluate tools/evaluate_uniform_aq.cpp)
target_link_libraries(glyphrelay_uniform_aq_evaluate PRIVATE glyphrelay_core)

add_executable(glyphrelay_saliency_validation tools/evaluate_saliency_validation.cpp)
target_link_libraries(glyphrelay_saliency_validation PRIVATE glyphrelay_core)

if(MSVC)
  target_compile_options(glyphrelay_uniform_aq_evaluate PRIVATE /W4 /WX /permissive-)
  target_compile_options(glyphrelay_saliency_validation PRIVATE /W4 /WX /permissive-)
else()
  target_compile_options(glyphrelay_uniform_aq_evaluate PRIVATE
    -Wall -Wconversion -Werror -Wextra -Wpedantic -Wshadow)
  target_compile_options(glyphrelay_saliency_validation PRIVATE
    -Wall -Wconversion -Werror -Wextra -Wpedantic -Wshadow)
endif()
