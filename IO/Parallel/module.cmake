vtk_module(vtkIOParallel
  DEPENDS
    vtkParallelCore
    vtkFiltersParallel
    vtkIONetCDF
    vtkexodusII
  TEST_DEPENDS
    vtkTestingCore
    vtkTestingRendering
    vtkInteractionStyle
    vtkRenderingOpenGL
  )
