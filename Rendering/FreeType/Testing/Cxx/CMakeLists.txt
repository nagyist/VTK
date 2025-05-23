# add tests that do not require data or produce vector output
set(TestFreeTypeTextMapperNoMath_ARGS DATA{../Data/Fonts/DejaVuSans.ttf})
set(TestMathTextFreeTypeTextRendererNoMath_ARGS
  DATA{../Data/Fonts/DejaVuSans.ttf}
)
vtk_add_test_cxx(vtkRenderingFreeTypeCxxTests tests
  TestBillboardTextActor3D.cxx,LOOSE_VALID
  TestFTStringToPath.cxx
  TestFlagpoleLabel.cxx
  TestFreeTypeTextMapperNoMath.cxx
  TestFreeTypeTools.cxx,NO_VALID
  TestMathTextFreeTypeTextRendererNoMath.cxx
  TestTextActor.cxx,LOOSE_VALID
  TestTextActorTileScaling.cxx
  TestTextActor3D.cxx,LOOSE_VALID
  TestTextActorAlphaBlending.cxx,LOOSE_VALID
  TestTextActorDepthPeeling.cxx
  TestTextActor3DAlphaBlending.cxx
  TestTextActor3DDepthPeeling.cxx
  TestTextActorScaleModeProp.cxx
  TestTextMapper.cxx,LOOSE_VALID
  TestTextOpaquePass.cxx
  )

vtk_add_test_cxx(vtkRenderingFreeTypeCxxTests no_data_tests
  NO_DATA NO_VALID NO_OUTPUT
  TestTextBoundingBox.cxx
  )
list(APPEND tests
  ${no_data_tests})

# Check for vtkErrorMacro calls in these tests (mainly for empty inputs):
set_tests_properties(
  VTK::RenderingFreeTypeCxx-TestTextActor
  VTK::RenderingFreeTypeCxx-TestTextActor3D
  PROPERTIES FAIL_REGULAR_EXPRESSION "ERROR:"
)

# Check that matplotlib is available
include(../../../../CMake/FindPythonModules.cmake)
find_python_module(matplotlib MATPLOTLIB_FOUND)

if(MATPLOTLIB_FOUND AND TARGET VTK::RenderingMatplotlib)
  set(TestMathTextFonts_ARGS DATA{../Data/Fonts/DejaVuSans.ttf})
  set(TestMathTextFreeTypeTextRenderer_ARGS DATA{../Data/Fonts/DejaVuSans.ttf})
  set(TestFreeTypeTextMapper_ARGS DATA{../Data/Fonts/DejaVuSans.ttf})
  set(TestFreeTypeTextMapperWithColumns_ARGS DATA{../Data/Fonts/DejaVuSans.ttf})
  set(TestFontDPIScaling_ARGS DATA{../Data/Fonts/DejaVuSans.ttf})

  vtk_add_test_cxx(vtkRenderingFreeTypeCxxTests matplotlib_tests
    TestMathTextFonts.cxx
    TestMathTextFreeTypeTextRenderer.cxx
    TestFreeTypeTextMapper.cxx
    TestFreeTypeTextMapperWithColumns.cxx
    TestFontDPIScaling.cxx
    )
  list(APPEND tests
    ${matplotlib_tests})
endif()

vtk_test_cxx_executable(vtkRenderingFreeTypeCxxTests tests
  DISABLE_FLOATING_POINT_EXCEPTIONS
  RENDERING_FACTORY
  )
