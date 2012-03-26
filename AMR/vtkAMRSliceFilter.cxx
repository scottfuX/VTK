/*=========================================================================

 Program:   Visualization Toolkit
 Module:    vtkAMRSliceFilter.cxx

 Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
 All rights reserved.
 See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

 This software is distributed WITHOUT ANY WARRANTY; without even
 the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 PURPOSE.  See the above copyright notice for more information.

 =========================================================================*/

#include "vtkAMRSliceFilter.h"
#include "vtkObjectFactory.h"
#include "vtkDataObject.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkOverlappingAMR.h"
#include "vtkCell.h"
#include "vtkAMRUtilities.h"
#include "vtkPlane.h"
#include "vtkAMRBox.h"
#include "vtkUniformGrid.h"
#include "vtkStructuredData.h"
#include "vtkCellData.h"
#include "vtkPointData.h"
#include "vtkInformationIntegerKey.h"
#include "vtkCompositeDataPipeline.h"
#include "vtkMultiProcessController.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkDataArray.h"

#include "vtkTimerLog.h"

#include <cassert>
#include <algorithm>
#include <sstream>

vtkStandardNewMacro(vtkAMRSliceFilter);

vtkAMRSliceFilter::vtkAMRSliceFilter()
{
  this->SetNumberOfInputPorts( 1 );
  this->SetNumberOfOutputPorts( 1 );
  this->OffSetFromOrigin  = 0.0;
  this->Normal            = 1;
  this->ForwardUpstream   = 1;
  this->EnablePrefetching = 1;
  this->Controller        = vtkMultiProcessController::GetGlobalController();
  this->initialRequest    = true;
}

//------------------------------------------------------------------------------
vtkAMRSliceFilter::~vtkAMRSliceFilter()
{
  // TODO Auto-generated destructor stub
}

//------------------------------------------------------------------------------
void vtkAMRSliceFilter::PrintSelf( std::ostream &os, vtkIndent indent )
{
  this->Superclass::PrintSelf( os, indent );
}

//------------------------------------------------------------------------------
int vtkAMRSliceFilter::FillInputPortInformation(
    int vtkNotUsed(port), vtkInformation *info )
{
  info->Set(
   vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(),"vtkOverlappingAMR" );
  return 1;
}

//------------------------------------------------------------------------------
int vtkAMRSliceFilter::FillOutputPortInformation(
    int vtkNotUsed(port), vtkInformation *info )
{
  info->Set(
    vtkDataObject::DATA_TYPE_NAME(),"vtkOverlappingAMR");
  return 1;
}

//------------------------------------------------------------------------------
bool vtkAMRSliceFilter::IsAMRData2D( vtkOverlappingAMR *input )
{
  assert( "pre: Input AMR dataset is NULL" && (input != NULL)  );

  vtkAMRBox box;
  input->GetMetaData( 0, 0, box );

  if( box.GetDimensionality() == 2 )
    {
    return true;
    }

  return false;
}

//------------------------------------------------------------------------------
void vtkAMRSliceFilter::InitializeOffSet(
    vtkOverlappingAMR* vtkNotUsed(inp), double *minBounds, double *maxBounds )
{
  if( !this->initialRequest )
    {
    return;
    }

  // Catch case where the normal is not set, this happens when the slice filter
  // is called from within RequestInformation.
  if( this->Normal == 0 )
    {
    this->Normal=1;
    }

  switch( this->Normal )
    {
    case 1:
      this->OffSetFromOrigin = ( maxBounds[0]-minBounds[0] )/2.0;
      break;
    case 2:
      this->OffSetFromOrigin = ( maxBounds[1]-minBounds[1] )/2.0;
      break;
    case 3:
      this->OffSetFromOrigin = ( maxBounds[2]-minBounds[2] )/2.0;
      break;
    default:
      vtkErrorMacro( "Undefined plane normal" );
    }
  this->initialRequest = false;
}

//------------------------------------------------------------------------------
vtkPlane* vtkAMRSliceFilter::GetCutPlane( vtkOverlappingAMR *inp )
{
  assert( "pre: AMR dataset should not be NULL" && (inp != NULL) );

  vtkTimerLog::MarkStartEvent( "AMRSlice::GetCutPlane" );

  vtkPlane *pl = vtkPlane::New();

  // Get global bounds
  double minBounds[3];
  double maxBounds[3];
  vtkAMRBox root;
  inp->GetRootAMRBox( root );
  root.GetMinBounds( minBounds );
  root.GetMaxBounds( maxBounds );

  double porigin[3];
  for( int i=0; i < 3; ++i )
    porigin[i]=minBounds[i];

  // Ensures the initial cut-plane is in the middle of the domain.
  this->InitializeOffSet( inp, minBounds, maxBounds );

  switch( this->Normal )
    {
    case 1:
      // X-Normal
      pl->SetNormal(1.0,0.0,0.0);
      porigin[0] += this->OffSetFromOrigin;
      break;
    case 2:
      // Y-Normal
      pl->SetNormal(0.0,1.0,0.0);
      porigin[1] += this->OffSetFromOrigin;
      break;
    case 3:
      // Z-Normal
      pl->SetNormal(0.0,0.0,1.0);
      porigin[2] += this->OffSetFromOrigin;
      break;
    default:
      vtkErrorMacro( "Undefined plane normal" );
    }
  pl->SetOrigin( porigin );

  vtkTimerLog::MarkEndEvent( "AMRSlice::GetCutPlane" );
  return( pl );

}

//------------------------------------------------------------------------------
vtkUniformGrid* vtkAMRSliceFilter::GetSlice(
    double porigin[3], vtkUniformGrid *grid )
{
  assert( "pre: input grid is NULL" && (grid != NULL) );
  assert( "pre: input grid must be a 3-D grid"&&(grid->GetDataDimension()==3));

//  vtkTimerLog::MarkStartEvent( "AMRSlice::GetSliceForBlock" );

  vtkUniformGrid *slice = vtkUniformGrid::New();

  // Get the 3-D Grid dimensions
  int dims[3];
  grid->GetDimensions( dims );

  // Storage for dimensions of the 2-D slice grid & its origin
  int    sliceDims[3];
  double sliceOrigin[3];

  switch( this->Normal )
    {
    case 1:
      // X-Normal -- YZ plane
      sliceDims[0] = 1;
      sliceDims[1] = dims[1];
      sliceDims[2] = dims[2];

      sliceOrigin[0] = porigin[0];
      sliceOrigin[1] = grid->GetOrigin()[1];
      sliceOrigin[2] = grid->GetOrigin()[2];

      slice->SetOrigin( sliceOrigin );
      slice->SetDimensions( sliceDims );
      slice->SetSpacing( grid->GetSpacing() );
      assert( slice->GetGridDescription()== VTK_YZ_PLANE );
      break;
    case 2:
      // Y-Normal -- XZ plane
      sliceDims[0] = dims[0];
      sliceDims[1] = 1;
      sliceDims[2] = dims[2];

      sliceOrigin[0] = grid->GetOrigin()[0];
      sliceOrigin[1] = porigin[1];
      sliceOrigin[2] = grid->GetOrigin()[2];

      slice->SetOrigin( sliceOrigin );
      slice->SetDimensions( sliceDims );
      slice->SetSpacing( grid->GetSpacing() );
      assert( slice->GetGridDescription() == VTK_XZ_PLANE );
      break;
    case 3:
      // Z-Normal -- XY plane
      sliceDims[0] = dims[0];
      sliceDims[1] = dims[1];
      sliceDims[2] = 1;

      sliceOrigin[0] = grid->GetOrigin()[0];
      sliceOrigin[1] = grid->GetOrigin()[1];
      sliceOrigin[2] = porigin[2];

      slice->SetOrigin( sliceOrigin );
      slice->SetDimensions( sliceDims );
      slice->SetSpacing( grid->GetSpacing() );
      assert( slice->GetGridDescription() == VTK_XY_PLANE );
      break;
    default:
      vtkErrorMacro( "Undefined normal" );
    }

//  vtkTimerLog::MarkEndEvent( "AMRSlice::GetSliceForBlock" );

  return( slice );
}

//------------------------------------------------------------------------------
bool vtkAMRSliceFilter::PlaneIntersectsAMRBox(double plane[4],double bounds[6])
{
  bool lowPnt  = false;
  bool highPnt = false;

  for( int i=0; i < 8; ++i )
    {
    // Get box coordinates
    double x = ( i&1 ? bounds[1] : bounds[0] );
    double y = ( i&2 ? bounds[3] : bounds[2] );
    double z = ( i&3 ? bounds[5] : bounds[4] );

    // Plug-in coordinates to the plane equation
    double v = plane[3] - plane[0]*x - plane[1]*y - plane[2]*z;

    if( v == 0.0 ) // Point is on a plane
      {
      return true;
      }

    if( v < 0.0 )
      {
      lowPnt = true;
      }
    else
      {
      highPnt = true;
      }
    if( lowPnt && highPnt )
      {
      return true;
      }
    }
  return false;
}

//------------------------------------------------------------------------------
void vtkAMRSliceFilter::ComputeAMRBlocksToLoad(
    vtkPlane *p, vtkOverlappingAMR *metadata )
{
  assert( "pre: plane object is NULL" && (p != NULL) );
  assert( "pre: metadata object is NULL" && (metadata != NULL) );

  vtkTimerLog::MarkStartEvent( "AMRSlice::ComputeAMRBlocksToLoad" );

  // Store A,B,C,D from the plane equation
  double plane[4];
  plane[0] = p->GetNormal()[0];
  plane[1] = p->GetNormal()[1];
  plane[2] = p->GetNormal()[2];
  plane[3] = p->GetNormal()[0]*p->GetOrigin()[0] +
             p->GetNormal()[1]*p->GetOrigin()[1] +
             p->GetNormal()[2]*p->GetOrigin()[2];

  double bounds[6];

  int maxLevelToLoad =
      (this->EnablePrefetching==1)? this->MaxResolution+1 : this->MaxResolution;

  unsigned int level=0;
  for( ; level <= static_cast<unsigned int>(maxLevelToLoad); ++level )
    {
    unsigned int dataIdx = 0;
    for( ; dataIdx < metadata->GetNumberOfDataSets( level ); ++dataIdx )
      {
      vtkAMRBox box;
      metadata->GetMetaData( level, dataIdx, box  );
      bounds[0] = box.GetMinX();
      bounds[1] = box.GetMaxX();
      bounds[2] = box.GetMinY();
      bounds[3] = box.GetMaxY();
      bounds[4] = box.GetMinZ();
      bounds[5] = box.GetMaxZ();

      if( this->PlaneIntersectsAMRBox( plane, bounds ) )
        {
        unsigned int amrGridIdx =
            metadata->GetCompositeIndex(level,dataIdx);
        this->blocksToLoad.push_back( amrGridIdx );
        }
      } // END for all data
    } // END for all levels

    std::sort( this->blocksToLoad.begin(), this->blocksToLoad.end() );

    vtkTimerLog::MarkEndEvent( "AMRSlice::ComputeAMRBlocksToLoad" );

//    std::cout << "Resolution-" << this->MaxResolution << ": ";
//    std::cout << this->blocksToLoad.size() << std::endl;
//    std::cout.flush();
}

//------------------------------------------------------------------------------
void vtkAMRSliceFilter::GetAMRSliceInPlane(
    vtkPlane *p, vtkOverlappingAMR *inp,
    vtkOverlappingAMR *out )
{
  assert( "pre: input AMR dataset is NULL" && (inp != NULL) );
  assert( "pre: output AMR dataset is NULL" && (out != NULL) );
  assert( "pre: cut plane is NULL" && (p != NULL) );

  vtkTimerLog::MarkStartEvent( "AMRSlice::GetAMRSliceInPlane" );

  // Store A,B,C,D from the plane equation
  double plane[4];
  plane[0] = p->GetNormal()[0];
  plane[1] = p->GetNormal()[1];
  plane[2] = p->GetNormal()[2];
  plane[3] = p->GetNormal()[0]*p->GetOrigin()[0] +
             p->GetNormal()[1]*p->GetOrigin()[1] +
             p->GetNormal()[2]*p->GetOrigin()[2];

  // Storage for the AMR box bounds
  double bounds[6];

  int NumLevels = inp->GetNumberOfLevels();
  int maxLevel =
    (this->MaxResolution < NumLevels )?
        this->MaxResolution+1 : NumLevels;

  unsigned int level=0;
  for( ; level < static_cast<unsigned int>(maxLevel); ++level )
    {
    unsigned int dataIdx=0;
    for( ; dataIdx < inp->GetNumberOfDataSets(level); ++dataIdx )
      {
      vtkAMRBox box;
      inp->GetMetaData( level, dataIdx, box );

      vtkUniformGrid *grid = inp->GetDataSet( level, dataIdx );
      if( grid != NULL )
        {
        bounds[0] = box.GetMinX();
        bounds[1] = box.GetMaxX();
        bounds[2] = box.GetMinY();
        bounds[3] = box.GetMaxY();
        bounds[4] = box.GetMinZ();
        bounds[5] = box.GetMaxZ();

        if( this->PlaneIntersectsAMRBox( plane, bounds ) )
          {
          vtkUniformGrid *slice = this->GetSlice( p->GetOrigin(), grid );
          assert( "Dimension of slice must be 2-D" &&
                   (slice->GetDataDimension()==2) );
          assert( "2-D slice is NULL" && (slice != NULL) );
          this->GetSliceCellData( slice, grid );
          unsigned int blockIdx =
              out->GetNumberOfDataSets( box.GetLevel() );
          out->SetDataSet( box.GetLevel(), blockIdx, slice );
          slice->Delete();
          } // END if plane intersects
        } // END if grid is not NULL
        else
          {
          // This belongs to another process
          unsigned int blockIdx=out->GetNumberOfDataSets( box.GetLevel() );
          out->SetDataSet( box.GetLevel(), blockIdx, NULL );
          } // END else
        } // END for all data
    } // END for all levels
  vtkTimerLog::MarkEndEvent( "AMRSlice::GetAMRSliceInPlane" );

  vtkTimerLog::MarkStartEvent( "AMRSlice::GenerateMetaData" );
  vtkAMRUtilities::GenerateMetaData( out, this->Controller );
  vtkTimerLog::MarkEndEvent( "AMRSlice::GenerateMetaData");

  vtkTimerLog::MarkStartEvent( "AMRSlice::GenerateVisibility" );
  out->GenerateVisibilityArrays();
  vtkTimerLog::MarkEndEvent( "AMRSlice::GenerateVisibility" );
}

//------------------------------------------------------------------------------
void vtkAMRSliceFilter::ComputeCellCenter(
    vtkUniformGrid *ug, const int cellIdx, double centroid[3] )
{
    assert( "pre: Input grid is NULL" && (ug != NULL) );
    assert( "pre: cell index out-of-bounds!" &&
            ( (cellIdx >= 0) && (cellIdx < ug->GetNumberOfCells() ) ) );

    vtkCell *myCell = ug->GetCell( cellIdx );
    assert( "post: cell is NULL" && (myCell != NULL) );

    myCell->GetNumberOfPoints();
    double pCenter[3];
    double *weights = new double[ myCell->GetNumberOfPoints() ];
    int subId       = myCell->GetParametricCenter( pCenter );
    myCell->EvaluateLocation( subId, pCenter, centroid, weights );
    delete [] weights;
//    myCell->Delete();
}

//------------------------------------------------------------------------------
int vtkAMRSliceFilter::GetDonorCellIdx( double x[3], vtkUniformGrid *ug )
{
  double x0[3];
  ug->GetOrigin( x0 );

  double h[3];
  ug->GetSpacing( h );

  int ijk[3];
  for( int i=0; i < 3; ++i )
    {
    ijk[ i ] = floor( (x[i]-x0[i])/h[i] );
    }

  int dims[3];
  ug->GetDimensions( dims );
  --dims[0]; --dims[1]; --dims[2];
  dims[0] = ( dims[0] < 1 )? 1 : dims[0];
  dims[1] = ( dims[1] < 1 )? 1 : dims[1];
  dims[2] = ( dims[2] < 1 )? 1 : dims[2];

  for( int i=0; i < 3; ++i )
    {
    if( ijk[i] < 0 || ijk[i] > dims[i] )
      {
      std::cerr << "Pnt: "<< x[0]  << " "<< x[1]  << " "<< x[2]  << "\n";
      std::cerr << "IJK: "<< ijk[0]<< " "<< ijk[1]<< " "<< ijk[2]<< "\n";
      std::cerr.flush();
      return -1;
      }
    }

  return( vtkStructuredData::ComputePointId( dims, ijk ) );
}

//------------------------------------------------------------------------------
void vtkAMRSliceFilter::GetSliceCellData(
    vtkUniformGrid *slice, vtkUniformGrid *grid3D )
{
  assert( "pre: AMR slice grid is NULL" && (slice != NULL) );
  assert( "pre: 3-D AMR slice grid is NULL" && (grid3D != NULL) );

//  vtkTimerLog::MarkStartEvent( "AMRSlice::GetDataForBlock" );

  // STEP 1: Allocate data-structures
  vtkCellData *sourceCD = grid3D->GetCellData();
  assert( "pre: source cell data is NULL" && ( sourceCD != NULL ) );

  vtkCellData *targetCD = slice->GetCellData();
  assert( "pre: target cell data is NULL" && (targetCD != NULL) );

  if( sourceCD->GetNumberOfArrays() == 0 )
    return;

  int numCells = slice->GetNumberOfCells();
  for( int arrayIdx=0; arrayIdx < sourceCD->GetNumberOfArrays(); ++arrayIdx )
    {
    vtkDataArray *array = sourceCD->GetArray( arrayIdx )->NewInstance();
    array->Initialize();
    array->SetName( sourceCD->GetArray( arrayIdx )->GetName() );
    array->SetNumberOfComponents(
     sourceCD->GetArray( arrayIdx )->GetNumberOfComponents( ) );
    array->SetNumberOfTuples( numCells );
    targetCD->AddArray( array );
    array->Delete();
    } // END for all arrays

  // STEP 2: Fill in slice data-arrays
  int numOrphans = 0;
  for( int cellIdx=0; cellIdx < numCells; ++cellIdx )
    {
    double probePnt[3];
    this->ComputeCellCenter( slice, cellIdx, probePnt );

    int sourceCellIdx = this->GetDonorCellIdx( probePnt, grid3D );
    if( sourceCellIdx != -1 )
      {
       int arrayIdx = 0;
       for(;arrayIdx < sourceCD->GetNumberOfArrays(); ++arrayIdx )
         {
         vtkDataArray *sourceArray = sourceCD->GetArray(arrayIdx);
         assert( "pre: src array is NULL" &&
                 (sourceArray != NULL) );
         const char *name = sourceArray->GetName();
         assert( "pre: target cell data must have array" &&
                  (targetCD->HasArray( name ) ) );
         vtkDataArray *targetArray = targetCD->GetArray( name );
         assert( "pre: target array is NULL" &&
                 (targetArray != NULL ) );
         assert( "pre: numcom mismatch" &&
                 (sourceArray->GetNumberOfComponents()==
                  targetArray->GetNumberOfComponents() ) );

         targetArray->SetTuple( cellIdx, sourceCellIdx, sourceArray );
         } // END for all arrays
      } // If a source cell is found, copy it's data.
    else
      {
      vtkGenericWarningMacro( "No Source cell found!" );
      ++ numOrphans;
      }
    } // END for all cells

    if( numOrphans != 0 )
      {
      vtkGenericWarningMacro(
          "Orphans: " << numOrphans << " / " << numCells );
      }

//    vtkTimerLog::MarkEndEvent( "AMRSlice::GetDataForBlock" );

}

//------------------------------------------------------------------------------
int vtkAMRSliceFilter::RequestInformation(
    vtkInformation* vtkNotUsed(rqst),
    vtkInformationVector **inputVector,
    vtkInformationVector* vtkNotUsed(outputVector) )
{
  this->blocksToLoad.clear();

  if( this->ForwardUpstream == 1 )
    {
    vtkInformation *input = inputVector[0]->GetInformationObject( 0 );
    assert( "pre: input information object is NULL" && (input != NULL) );

    // Check if metadata are passed downstream
    if( input->Has(vtkCompositeDataPipeline::COMPOSITE_DATA_META_DATA() ) )
      {
      vtkOverlappingAMR *metadata =
          vtkOverlappingAMR::SafeDownCast(
              input->Get(
                  vtkCompositeDataPipeline::COMPOSITE_DATA_META_DATA( ) ) );

      vtkPlane *cutPlane = this->GetCutPlane( metadata );
      assert( "Cut plane is NULL" && (cutPlane != NULL) );

      this->ComputeAMRBlocksToLoad( cutPlane, metadata );
      cutPlane->Delete();
      }
    }

  this->Modified();
  return 1;
}

//------------------------------------------------------------------------------
int vtkAMRSliceFilter::RequestUpdateExtent(
    vtkInformation*, vtkInformationVector **inputVector,
    vtkInformationVector* vtkNotUsed(outputVector) )
{

  if( this->ForwardUpstream == 1 )
    {
    vtkInformation * inInfo = inputVector[0]->GetInformationObject(0);
    assert( "pre: inInfo is NULL" && (inInfo != NULL)  );

    // Send upstream request for higher resolution
    inInfo->Set(
     vtkCompositeDataPipeline::UPDATE_COMPOSITE_INDICES(),
     &this->blocksToLoad[0], static_cast<int>(this->blocksToLoad.size()));
    }

  return 1;
}

//------------------------------------------------------------------------------
int vtkAMRSliceFilter::RequestData(
    vtkInformation* vtkNotUsed(request), vtkInformationVector** inputVector,
    vtkInformationVector* outputVector )
{
  std::ostringstream oss;
  oss.clear();
  oss << "AMRSlice::Request-" << this->MaxResolution;

  std::string eventName = oss.str();
  vtkTimerLog::MarkStartEvent( eventName.c_str() );

  // STEP 0: Get input object
  vtkInformation *input = inputVector[0]->GetInformationObject( 0 );
  assert( "pre: input information object is NULL" && (input != NULL) );


  vtkOverlappingAMR *inputAMR=
      vtkOverlappingAMR::SafeDownCast(
          input->Get(vtkDataObject::DATA_OBJECT() ) );

  // STEP 1: Get output object
  vtkInformation *output = outputVector->GetInformationObject( 0 );
  assert( "pre: output information object is NULL" && (output != NULL) );
  vtkOverlappingAMR *outputAMR=
      vtkOverlappingAMR::SafeDownCast(
        output->Get( vtkDataObject::DATA_OBJECT() ) );

  if( this->IsAMRData2D( inputAMR ) )
    {
    outputAMR->ShallowCopy( inputAMR );
    return 1;
    }

  // STEP 2: Compute global origin
  vtkPlane *cutPlane = this->GetCutPlane( inputAMR );
  assert( "Cut plane is NULL" && (cutPlane != NULL) );

  // STEP 3: Get the AMR slice
  this->GetAMRSliceInPlane( cutPlane, inputAMR, outputAMR );
  cutPlane->Delete();

  vtkTimerLog::MarkEndEvent( eventName.c_str() );
  return 1;
}
