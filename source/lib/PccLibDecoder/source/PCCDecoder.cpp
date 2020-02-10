/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2017, ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "PCCCommon.h"
#include "PCCHighLevelSyntax.h"
#include "PCCBitstream.h"
#include "PCCVideoBitstream.h"
#include "PCCContext.h"
#include "PCCFrameContext.h"
#include "PCCPatch.h"
#include "PCCVideoDecoder.h"
#include "PCCGroupOfFrames.h"
#include <tbb/tbb.h>
#include "PCCDecoder.h"

using namespace pcc;
using namespace std;

PCCDecoder::PCCDecoder() {
#ifdef ENABLE_PAPI_PROFILING
  initPapiProfiler();
#endif
}
PCCDecoder::~PCCDecoder() {}

void PCCDecoder::setParameters( PCCDecoderParameters params ) { params_ = params; }

int PCCDecoder::decode( PCCContext& context, PCCGroupOfFrames& reconstructs ) {
  if ( params_.nbThread_ > 0 ) { tbb::task_scheduler_init init( (int)params_.nbThread_ ); }
#ifdef CODEC_TRACE
    setTrace( true );
    openTrace( stringFormat( "%s_GOF%u_patch_decode.txt", removeFileExtension( params_.compressedStreamPath_ ).c_str(),
                             context.getVps().getVpccParameterSetId() ) );
#endif
  createPatchFrameDataStructure( context );
#ifdef CODEC_TRACE
  closeTrace();
#endif

  PCCVideoDecoder   videoDecoder;
  std::stringstream path;
  auto&             sps        = context.getVps();
  int32_t           atlasIndex = 0;
  auto&             ai         = sps.getAttributeInformation( atlasIndex );
  auto&             oi         = sps.getOccupancyInformation( atlasIndex );
  auto&             gi         = sps.getGeometryInformation( atlasIndex );
  auto&             asps       = context.getAtlasSequenceParameterSet( 0 );
  path << removeFileExtension( params_.compressedStreamPath_ ) << "_dec_GOF" << sps.getVpccParameterSetId() << "_";
#ifdef CODEC_TRACE
  setTrace( true );
  openTrace( stringFormat( "%s_GOF%u_codec_decode.txt", removeFileExtension( params_.compressedStreamPath_ ).c_str(),
                           sps.getVpccParameterSetId() ) );
#endif
  bool         lossyMpp           = !sps.getLosslessGeo() && sps.getRawPatchEnabledFlag( atlasIndex );
  const size_t frameCountGeometry = sps.getMapCountMinus1( atlasIndex ) + 1;
  const size_t mapCount           = sps.getMapCountMinus1( atlasIndex ) + 1;
  auto&        videoBitstreamOM   = context.getVideoBitstream( VIDEO_OCCUPANCY );
  int          decodedBitDepthOM  = 8;
  reconstructs.resize( context.size() );

  videoDecoder.decompress( context.getVideoOccupancyMap(), path.str(), context.size(), videoBitstreamOM,
                           params_.videoDecoderOccupancyMapPath_, context, decodedBitDepthOM,
                           params_.keepIntermediateFiles_, ( sps.getLosslessGeo() ? sps.getLosslessGeo444() : false ),
                           false, "", "" );
  auto& videoOcc = context.getVideoOccupancyMap();
  // converting the decoded bitdepth to the nominal bitdepth
  context.getVideoOccupancyMap().convertBitdepth( decodedBitDepthOM, oi.getOccupancyNominal2DBitdepthMinus1() + 1,
                                                  oi.getOccupancyMSBAlignFlag() );
  context.setOccupancyPrecision( sps.getFrameWidth( atlasIndex ) / context.getVideoOccupancyMap().getWidth() );

  bool seiSmootingIsPresent = context.seiIsPresent( NAL_PREFIX_SEI, SMOOTHING_PARAMETERS );  
  bool pbfEnableFlag = false; 
  if( seiSmootingIsPresent ) {
    SEISmoothingParameters& sei =
        static_cast<SEISmoothingParameters&>( context.getSei( NAL_PREFIX_SEI, SMOOTHING_PARAMETERS ) );
    pbfEnableFlag = sei.getSpGeometrySmoothingEnabledFlag() && sei.getSpGeometrySmoothingId() == 1;   
  }
  if ( !pbfEnableFlag ) {
    generateOccupancyMap( context, context.getOccupancyPrecision(), oi.getLossyOccupancyMapCompressionThreshold(),
                          asps.getEnhancedOccupancyMapForDepthFlag() );
  }

  if ( sps.getMultipleMapStreamsPresentFlag( atlasIndex ) ) {
    if ( lossyMpp ) {
      std::cout << "ERROR! Lossy-missed-points-patch code not implemented when absoluteD_ = 0 as "
                   "of now. Exiting ..."
                << std::endl;
      std::exit( -1 );
    }
    context.getVideoGeometryMultiple().resize(sps.getMapCountMinus1(atlasIndex) + 1);
    size_t totalGeoSize = 0;
    for (int mapIndex = 0; mapIndex < sps.getMapCountMinus1(atlasIndex) + 1; mapIndex++) {
      // Decompress D[mapIndex]
      int   decodedBitDepth = gi.getGeometryNominal2dBitdepthMinus1() + 1; // this should be extracted from the bitstream
      PCCVideoType geometryIndex = (PCCVideoType)(VIDEO_GEOMETRY_D0 + mapIndex);
      auto& videoBitstream = context.getVideoBitstream( geometryIndex );
      videoDecoder.decompress( context.getVideoGeometryMultiple()[mapIndex], path.str(), context.size(), videoBitstream,
                               params_.videoDecoderPath_, context, decodedBitDepth, params_.keepIntermediateFiles_,
                             ( sps.getLosslessGeo() ? sps.getLosslessGeo444() : false ) );
      context.getVideoGeometryMultiple()[mapIndex].convertBitdepth( decodedBitDepth, gi.getGeometryNominal2dBitdepthMinus1() + 1,
                                                gi.getGeometryMSBAlignFlag() );
      std::cout << "geometry D"<<mapIndex<<" video ->" << videoBitstream.size() << " B" << std::endl;
      totalGeoSize += videoBitstream.size();
    }    
    std::cout << "total geometry video ->" << totalGeoSize << " B"
              << std::endl;
  } else {
    int   decodedBitDepthGeo = gi.getGeometryNominal2dBitdepthMinus1() + 1;
    auto& videoBitstream     = context.getVideoBitstream( VIDEO_GEOMETRY );
    videoDecoder.decompress( context.getVideoGeometry(), path.str(), context.size() * frameCountGeometry,
                             videoBitstream, params_.videoDecoderPath_, context, decodedBitDepthGeo,
                             params_.keepIntermediateFiles_, sps.getLosslessGeo() & sps.getLosslessGeo444() );
    context.getVideoGeometry().convertBitdepth( decodedBitDepthGeo, gi.getGeometryNominal2dBitdepthMinus1() + 1,
                                                gi.getGeometryMSBAlignFlag() );
    std::cout << "geometry video ->" << videoBitstream.size() << " B" << std::endl;
  }

  if ( sps.getRawPatchEnabledFlag( atlasIndex ) && sps.getRawSeparateVideoPresentFlag( atlasIndex ) ) {
    int   decodedBitDepthMP = gi.getGeometryNominal2dBitdepthMinus1() + 1;
    auto& videoBitstreamMP  = context.getVideoBitstream( VIDEO_GEOMETRY_RAW );
    videoDecoder.decompress( context.getVideoMPsGeometry(), path.str(), context.size(), videoBitstreamMP,
                             params_.videoDecoderPath_, context, decodedBitDepthMP, params_.keepIntermediateFiles_ );
    context.getVideoMPsGeometry().convertBitdepth( decodedBitDepthMP, gi.getGeometryNominal2dBitdepthMinus1() + 1,
                                                   gi.getGeometryMSBAlignFlag() );
    generateMissedPointsGeometryfromVideo( context, reconstructs );
    std::cout << " missed points geometry -> " << videoBitstreamMP.size() << " B " << endl;
  }
  if ( asps.getEnhancedOccupancyMapForDepthFlag() && !pbfEnableFlag ) {
    generateBlockToPatchFromOccupancyMap( context, context.getOccupancyPackingBlockSize(), true );
  } else {
    generateBlockToPatchFromBoundaryBox( context, context.getOccupancyPackingBlockSize() );
  }

  GeneratePointCloudParameters gpcParams;
  setGeneratePointCloudParameters( gpcParams, context );
  std::vector<std::vector<uint32_t>> partitions;
  printf("generatePointCloud \n"); fflush(stdout);
  generatePointCloud( reconstructs, context, gpcParams, partitions, true );
  printf("generatePointCloud done\n"); fflush(stdout);

  if ( ai.getAttributeCount() > 0 ) {
    for (int attrIndex = 0; attrIndex < sps.getAttributeInformation(atlasIndex).getAttributeCount(); attrIndex++) {//right now we only have one attribute, this should be generalized
      int decodedBitdepthAttribute = ai.getAttributeNominal2dBitdepthMinus1(attrIndex) + 1;
      int   decodedBitdepthAttributeMP = ai.getAttributeNominal2dBitdepthMinus1(attrIndex) + 1;
      for (int attrPartitionIndex = 0; attrPartitionIndex < sps.getAttributeInformation(atlasIndex).getAttributeDimensionPartitionsMinus1(attrIndex) + 1; attrPartitionIndex++) {//right now we have only one partition, this should be generalized
        if (sps.getMultipleMapStreamsPresentFlag(atlasIndex)) {
          int sizeTextureVideo = 0;
          context.getVideoTextureMultiple().resize(sps.getMapCountMinus1(atlasIndex) + 1); //this allocation is considering only one attribute, with a single partition, but multiple streams
          for (int mapIndex = 0; mapIndex < sps.getMapCountMinus1(atlasIndex) + 1; mapIndex++) {
            // decompress T[mapIndex]
            PCCVideoType textureIndex = (PCCVideoType)(VIDEO_TEXTURE_T0 + attrPartitionIndex + MAX_NUM_ATTR_PARTITIONS * mapIndex);
            auto& videoBitstream = context.getVideoBitstream(textureIndex);
            videoDecoder.decompress(context.getVideoTextureMultiple()[mapIndex], path.str(), context.size(), videoBitstream,
                               params_.videoDecoderPath_, context, ai.getAttributeNominal2dBitdepthMinus1( 0 ) + 1,
                               params_.keepIntermediateFiles_, sps.getLosslessGeo() != 0,
                               params_.patchColorSubsampling_, params_.inverseColorSpaceConversionConfig_,
                               params_.colorSpaceConversionPath_ );
            /*context.getVideoTextureMultiple()[mapIndex].convertBitdepth(
              decodedBitdepthAttribute, ai.getAttributeNominal2dBitdepthMinus1(0) + 1, ai.getAttributeMSBAlignFlag());*/
            std::cout << "texture T" << mapIndex << " video ->" << videoBitstream.size() << " B" << std::endl;
            sizeTextureVideo += videoBitstream.size();
          }
          std::cout << "texture    video ->" << sizeTextureVideo << " B" << std::endl;
        }
        else {
          PCCVideoType textureIndex = (PCCVideoType)(VIDEO_TEXTURE + attrPartitionIndex );
          auto& videoBitstream = context.getVideoBitstream(textureIndex);
          videoDecoder.decompress( context.getVideoTexture(),       // video,
                               path.str(),                      // path,
                               context.size() * mapCount,       // frameCount,
                               videoBitstream,                  // bitstream,
                               params_.videoDecoderPath_,       // decoderPath,
                               context,                         // contexts,
                               decodedBitdepthAttribute,        // bitDepth,
                               params_.keepIntermediateFiles_,  // keepIntermediateFiles
                               sps.getLosslessGeo() != 0,       // use444CodecIo
                               params_.patchColorSubsampling_,  // patchColorSubsampling
                               params_.inverseColorSpaceConversionConfig_, params_.colorSpaceConversionPath_ );
          /*context.getVideoTexture().convertBitdepth(
              decodedBitdepthAttribute, ai.getAttributeNominal2dBitdepthMinus1(0) + 1, ai.getAttributeMSBAlignFlag());*/
          std::cout << "texture video  ->" << videoBitstream.size() << " B" << std::endl;
        }
        if ( sps.getRawPatchEnabledFlag( atlasIndex ) && sps.getRawSeparateVideoPresentFlag( atlasIndex ) ) {
          PCCVideoType textureIndex = (PCCVideoType)(VIDEO_TEXTURE_RAW + attrPartitionIndex );
          auto& videoBitstreamMP = context.getVideoBitstream(textureIndex);
          videoDecoder.decompress( context.getVideoMPsTexture(), path.str(), context.size(), videoBitstreamMP,
                               params_.videoDecoderPath_, context, decodedBitdepthAttributeMP,
                               params_.keepIntermediateFiles_, sps.getLosslessGeo(), false,
                               params_.inverseColorSpaceConversionConfig_, params_.colorSpaceConversionPath_ );
          context.getVideoTexture().convertBitdepth(
          decodedBitdepthAttributeMP, ai.getAttributeNominal2dBitdepthMinus1( 0 ) + 1, ai.getAttributeMSBAlignFlag() );      
          generateMissedPointsTexturefromVideo( context, reconstructs );
          std::cout << " missed points texture -> " << videoBitstreamMP.size() << " B" << endl;
        }
      }
    }
  }
  colorPointCloud( reconstructs, context, ai.getAttributeCount(), params_.colorTransform_,
                   ai.getAttributeMapAbsoluteCodingEnabledFlagList(),  // atlasIdx
                   sps.getMultipleMapStreamsPresentFlag( ATLASIDXPCC ), gpcParams );

  //  Generate a buffer to keep unsmoothed geometry, then do geometry smoothing and transfer followed by color
  //  smoothing
  if ( gpcParams.flagGeometrySmoothing_ ) {
    if ( gpcParams.gridSmoothing_ ) {
      PCCGroupOfFrames tempFrameBuffer;
      auto&            frames = context.getFrames();
      tempFrameBuffer.resize( reconstructs.size() );
      for ( size_t i = 0; i < frames.size(); i++ ) { tempFrameBuffer[i] = reconstructs[i]; }
      smoothPointCloudPostprocess( reconstructs, context, params_.colorTransform_, gpcParams, partitions );
      for ( size_t i = 0; i < frames.size(); i++ ) {
        // These are different attribute transfer functions
        if ( params_.postprocessSmoothingFilter_ == 1 ) {
          //tempFrameBuffer[i].transferColors16bit( reconstructs[i], int32_t( 0 ), sps.getLosslessGeo() == 1, 8, 1, 1, 1, 1, 0,
          //                                   4, 4, 1000, 1000, 1000*256, 1000*256 );  // jkie: make it general
			tempFrameBuffer[i].transferColors16bitBP( reconstructs[i], int32_t( 0 ), sps.getLosslessGeo() == 1, 8, 1, 1, 1, 1, 0,
                                             4, 4, 1000, 1000, 1000*256, 1000*256 );  // jkie: make it general
        } else if ( params_.postprocessSmoothingFilter_ == 2 ) {
          tempFrameBuffer[i].transferColorWeight( reconstructs[i], 0.1 );
        } else if ( params_.postprocessSmoothingFilter_ == 3 ) {
          tempFrameBuffer[i].transferColorsFilter3( reconstructs[i], int32_t( 0 ), sps.getLosslessGeo() == 1 );
        }
      }
    }
  }
  //    This function does the color smoothing that is usually done in colorPointCloud
  if ( gpcParams.flagColorSmoothing_ ) { colorSmoothing( reconstructs, context, params_.colorTransform_, gpcParams );
  }
 
  auto& frames = context.getFrames();
  if ( sps.getLosslessGeo() != 1 ) {  // lossy: convert 16-bit yuv444 to 8-bit RGB444
  for ( size_t i = 0; i < frames.size(); i++ ) {
    for ( int k = 0; k < reconstructs[i].getPointCount(); k++ ) {
      convertYUV444_16bits_toRGB_8bits( reconstructs[i], k );
    }
  }
  } else {  // lossless: copy 16-bit RGB to 8-bit RGB
    PCCColor16bit color16;
    PCCColor3B    color8;
    for ( size_t i = 0; i < frames.size(); i++ ) {
      for ( int k = 0; k < reconstructs[i].getPointCount(); k++ ) {
        color16   = reconstructs[i].getColor16bit( k );
        color8[0] = uint8_t( color16[0] );
        color8[1] = uint8_t( color16[1] );
        color8[2] = uint8_t( color16[2] );
        reconstructs[i].setColor( k, color8 );
      }
    }
  }

#ifdef CODEC_TRACE
  setTrace( false );
  closeTrace();
#endif
  return 0;
}

void PCCDecoder::setPointLocalReconstruction( PCCContext& context ) {
  auto&                        asps = context.getAtlasSequenceParameterSet( 0 );  
  PointLocalReconstructionMode mode = {0, 0, 0, 1};
  context.addPointLocalReconstructionMode( mode );
  if( asps.getPointLocalReconstructionEnabledFlag() ) {
    auto& plri = asps.getPointLocalReconstructionInformation( 0 );
    for ( size_t i = 0; i < plri.getNumberOfModesMinus1(); i++ ) {
      mode.interpolate_ = plri.getInterpolateFlag( i );
      mode.filling_     = plri.getFillingFlag( i );
      mode.minD1_       = plri.getMinimumDepth( i );
      mode.neighbor_    = plri.getNeighbourMinus1( i ) + 1;
      context.addPointLocalReconstructionMode( mode );
    }
  }
#ifdef CODEC_TRACE
  for ( size_t i = 0; i < context.getPointLocalReconstructionModeNumber(); i++ ) {
    auto& mode = context.getPointLocalReconstructionMode( i );
    TRACE_CODEC( "Plrm[%u]: Inter = %d Fill = %d minD1 = %u neighbor = %u \n", i, mode.interpolate_, mode.filling_,
                 mode.minD1_, mode.neighbor_ );
  }
#endif
}

void PCCDecoder::setPointLocalReconstructionData( PCCFrameContext&              frame,
                                                  PCCPatch&                     patch,
                                                  PointLocalReconstructionData& plrd,
                                                  size_t                        occupancyPackingBlockSize ) {
  patch.allocOneLayerData();
  TRACE_CODEC( "WxH = %lu x %lu \n", plrd.getBlockToPatchMapWidth(), plrd.getBlockToPatchMapHeight() );
  patch.getPointLocalReconstructionLevel() = plrd.getLevelFlag();
  TRACE_CODEC( "  LevelFlag = %d \n", plrd.getLevelFlag() );
  if ( plrd.getLevelFlag() ) {
    if ( plrd.getPresentFlag() ) {
      patch.getPointLocalReconstructionMode() = plrd.getModeMinus1() + 1;
    } else {
      patch.getPointLocalReconstructionMode() = 0;
    }
    TRACE_CODEC( "  ModePatch: Present = %d ModeMinus1 = %2d \n", plrd.getPresentFlag(),
                 plrd.getPresentFlag() ? (int32_t)plrd.getModeMinus1() : -1 );
  } else {
    for ( size_t v0 = 0; v0 < plrd.getBlockToPatchMapHeight(); ++v0 ) {
      for ( size_t u0 = 0; u0 < plrd.getBlockToPatchMapWidth(); ++u0 ) {
        size_t index = v0 * plrd.getBlockToPatchMapWidth() + u0;
        if ( plrd.getBlockPresentFlag( index ) ) {
          patch.getPointLocalReconstructionMode( u0, v0 ) = plrd.getBlockModeMinus1( index ) + 1;
        } else {
          patch.getPointLocalReconstructionMode( u0, v0 ) = 0;
        }
        TRACE_CODEC( "  Mode[%3u]: Present = %d ModeMinus1 = %2d \n", index, plrd.getBlockPresentFlag( index ),
                     plrd.getBlockPresentFlag( index ) ? (int32_t)plrd.getBlockModeMinus1( index ) : -1 );
      }
    }
  }
#ifdef CODEC_TRACE
  for ( size_t v0 = 0; v0 < patch.getSizeV0(); ++v0 ) {
    for ( size_t u0 = 0; u0 < patch.getSizeU0(); ++u0 ) {
      TRACE_CODEC( "Block[ %2lu %2lu <=> %4lu ] / [ %2lu %2lu ]: Level = %d Present = %d mode = %lu \n", u0, v0,
                   v0 * patch.getSizeU0() + u0, patch.getSizeU0(), patch.getSizeV0(),
                   patch.getPointLocalReconstructionLevel(), plrd.getBlockPresentFlag( v0 * patch.getSizeU0() + u0 ),
                   patch.getPointLocalReconstructionMode( u0, v0 ) );
    }
  }
#endif
}

void PCCDecoder::setGeneratePointCloudParameters( GeneratePointCloudParameters& params, PCCContext& context ) {
  auto&   sps                   = context.getVps();
  int32_t atlasIndex            = 0;
  auto&   ai                    = sps.getAttributeInformation( atlasIndex );
  auto&   oi                    = sps.getOccupancyInformation( atlasIndex );
  auto&   gi                    = sps.getGeometryInformation( atlasIndex );
  auto&   asps                  = context.getAtlasSequenceParameterSet( 0 );
  bool    seiSmootingIsPresent  = context.seiIsPresent( NAL_PREFIX_SEI, SMOOTHING_PARAMETERS );
  params.flagGeometrySmoothing_ = false;
  params.gridSmoothing_         = false;
  params.gridSize_              = 0;
  params.thresholdSmoothing_    = 0;
  params.pbfEnableFlag_         = false;
  params.pbfPassesCount_        = 0;
  params.pbfFilterSize_         = 0;
  params.pbfLog2Threshold_      = 0;
  printf( "params.pbfLog2Threshold_      = 0; \n " );
  if ( seiSmootingIsPresent ) {
    SEISmoothingParameters& sei =
        static_cast<SEISmoothingParameters&>( context.getSei( NAL_PREFIX_SEI, SMOOTHING_PARAMETERS ) );
    if ( sei.getSpGeometrySmoothingEnabledFlag() ) {
      params.flagGeometrySmoothing_ = true;
      if ( sei.getSpGeometrySmoothingId() == 0 ) {
        params.gridSmoothing_      = true;
        params.gridSize_           = sei.getSpGeometrySmoothingGridSizeMinus2() + 2;
        params.thresholdSmoothing_ = (double)sei.getSpGeometrySmoothingThreshold();
      }
      if ( sei.getSpGeometrySmoothingId() == 1 ) {
        params.pbfEnableFlag_    = true;
        params.pbfPassesCount_   = sei.getSpGeometryPatchBlockFilteringPassesCountMinus1()   + 1;
        params.pbfFilterSize_    = sei.getSpGeometryPatchBlockFilteringFilterSizeMinus1()    + 1;
        params.pbfLog2Threshold_ = sei.getSpGeometryPatchBlockFilteringLog2ThresholdMinus1() + 1;
      }
    }
  }
  params.occupancyResolution_    = context.getOccupancyPackingBlockSize();
  params.occupancyPrecision_     = context.getOccupancyPrecision();
  params.enableSizeQuantization_ = context.getAtlasSequenceParameterSet( 0 ).getPatchSizeQuantizerPresentFlag();
  params.rawPointColorFormat_    = size_t( sps.getLosslessGeo444() != 0 ? COLOURFORMAT444 : COLOURFORMAT420 );
  params.nbThread_               = params_.nbThread_;
  params.absoluteD1_ = sps.getMapCountMinus1( atlasIndex ) == 0 || sps.getMapAbsoluteCodingEnableFlag( atlasIndex, 1 );
  params.multipleStreams_               = sps.getMultipleMapStreamsPresentFlag( atlasIndex );
  params.surfaceThickness_              = asps.getSurfaceThicknessMinus1() + 1;
  params.thresholdColorSmoothing_       = 0.;
  params.gridColorSmoothing_            = false;
  params.cgridSize_                     = 0;
  params.thresholdColorDifference_      = 0;
  params.thresholdColorVariation_       = 0;
  params.thresholdLocalEntropy_         = 0;
  params.radius2ColorSmoothing_         = 64;
  params.neighborCountColorSmoothing_   = 64;
  params.flagColorSmoothing_            = 0; 
  if ( seiSmootingIsPresent ) {
    SEISmoothingParameters& sei =
        static_cast<SEISmoothingParameters&>( context.getSei( NAL_PREFIX_SEI, SMOOTHING_PARAMETERS ) );
    for ( size_t j = 0; j < sei.getSpNumAttributeUpdates(); j++ ) {
      size_t index = sei.getSpAttributeIdx( j );
      for ( size_t i = 0; i < sei.getSpDimensionMinus1( index ) + 1; i++ ) {
        if ( sei.getSpAttrSmoothingParamsEnabledFlag( index, i ) ) {
          params.flagColorSmoothing_       = true;
          params.thresholdColorSmoothing_  = (double)sei.getSpAttrSmoothingThreshold( index, i );  
          params.gridColorSmoothing_       = true;
          params.cgridSize_                = sei.getSpAttrSmoothingGridSizeMinus2( index, i ) + 2;    
          params.thresholdColorDifference_ = sei.getSpAttrSmoothingThresholdDifference( index, i );   
          params.thresholdColorVariation_  = sei.getSpAttrSmoothingThresholdVariation( index, i );     
          params.thresholdLocalEntropy_    = sei.getSpAttrSmoothingLocalEntropyThreshold( index, i );  
        }
      }
    }
  }
  params.thresholdLossyOM_              = (size_t)oi.getLossyOccupancyMapCompressionThreshold();
  params.removeDuplicatePoints_         = asps.getRemoveDuplicatePointEnabledFlag();
  params.pointLocalReconstruction_      = asps.getPointLocalReconstructionEnabledFlag();
  params.mapCountMinus1_                = sps.getMapCountMinus1( atlasIndex );
  params.singleMapPixelInterleaving_    = asps.getPixelDeinterleavingFlag();
  params.useAdditionalPointsPatch_      = sps.getRawPatchEnabledFlag( atlasIndex );
  params.enhancedDeltaDepthCode_        = asps.getEnhancedOccupancyMapForDepthFlag();
  params.EOMFixBitCount_                = asps.getEnhancedOccupancyMapFixBitCountMinus1() + 1;
  params.geometry3dCoordinatesBitdepth_ = gi.getGeometry3dCoordinatesBitdepthMinus1() + 1;
  params.geometryBitDepth3D_            = gi.getGeometry3dCoordinatesBitdepthMinus1() + 1;
  printf("Params: SPI = %d PBF = %d \n",params.singleMapPixelInterleaving_,params.pbfEnableFlag_); fflush(stdout);
}

void PCCDecoder::createPatchFrameDataStructure( PCCContext& context ) {
  TRACE_CODEC( "createPatchFrameDataStructure GOP start \n" );
  auto&  sps        = context.getVps();
  size_t atlasIndex = 0;
  auto&  asps       = context.getAtlasSequenceParameterSet( 0 );
  auto&  atglulist  = context.getAtlasTileGroupLayerList();
  context.setOccupancyPackingBlockSize( pow( 2, asps.getLog2PatchPackingBlockSize() ) );
  context.resize( atglulist.size() );
  TRACE_CODEC( "frameCount = %u \n", context.size() );
  setPointLocalReconstruction( context );
  context.constructRefList(0, 0);
  context.setMPGeoWidth( 64 );
  context.setMPAttWidth( 0 );
  context.setMPGeoHeight( 0 );
  context.setMPAttHeight( 0 );

  for ( int i = 0; i < context.size(); i++ ) {
    auto& frame = context.getFrame( i );
    if(i>0) { frame.setRefAFOCList( context ); }
    frame.setAFOC(i); 
    frame.setIndex( i );
    frame.setWidth( sps.getFrameWidth( atlasIndex ) );
    frame.setHeight( sps.getFrameHeight( atlasIndex ) );
    frame.setLosslessGeo( sps.getLosslessGeo() );
    frame.setLosslessGeo444( sps.getLosslessGeo444() );
    frame.setUseMissedPointsSeparateVideo( sps.getRawSeparateVideoPresentFlag( atlasIndex ) );
    frame.setRawPatchEnabledFlag( sps.getRawPatchEnabledFlag( atlasIndex ) );
    createPatchFrameDataStructure( context, frame, i );
  }
}
void PCCDecoder::createPatchFrameDataStructure( PCCContext&      context,
                                                PCCFrameContext& frame,
                                                size_t           frameIndex ) {
  TRACE_CODEC( "createPatchFrameDataStructure Frame %lu \n", frame.getIndex() );
  auto&  sps        = context.getVps();
  size_t atlasIndex = 0;
  auto&  gi         = context.getVps().getGeometryInformation( atlasIndex );

  auto& asps  = context.getAtlasSequenceParameterSet( 0 );
  auto& afps  = context.getAtlasFrameParameterSet( 0 );
  auto& atglu = context.getAtlasTileGroupLayer( frameIndex );
  auto& atgh  = atglu.getAtlasTileGroupHeader();
  auto& atgdu = atglu.getAtlasTileGroupDataUnit();

  auto&        patches        = frame.getPatches();
  auto&        pcmPatches     = frame.getMissedPointsPatches();
  auto&        eomPatches     = frame.getEomPatches();
  int64_t      prevSizeU0     = 0;
  int64_t      prevSizeV0     = 0;
  int64_t      prevPatchSize2DXInPixel = 0;
  int64_t      prevPatchSize2DYInPixel = 0;
  int64_t      predIndex      = 0;
  const size_t minLevel       = sps.getMinLevel();
  size_t       numRawPatches  = 0;
  size_t       numNonRawPatch = 0;
  size_t       numEomPatch    = 0;
  PCCTILEGROUP tileGroupType  = atgh.getAtghType();
  size_t       patchCount     = atgdu.getPatchCount();
  for ( size_t i = 0; i < patchCount; i++ ) {
    PCCPatchType currPatchType=getCurrPatchType(tileGroupType,atgdu.getPatchMode( i ));
    if     ( currPatchType==RAW_PATCH ) numRawPatches++;
    else if( currPatchType==EOM_PATCH ) numEomPatch++;
  }
  numNonRawPatch = patchCount - numRawPatches - numEomPatch;
  eomPatches.reserve( numEomPatch );
  patches.resize( numNonRawPatch );
  pcmPatches.resize( numRawPatches );
  TRACE_CODEC( "Patches size                        = %lu \n", patches.size() );
  TRACE_CODEC( "non-regular Patches(pcm, eom)     = %lu, %lu \n", numRawPatches, numEomPatch );
  TRACE_CODEC( "TileGroup Type                     = %zu (0.P_TILE_GRP 1.SKIP_TILE_GRP 2.I_TILE_GRP)\n",
               (size_t)atgh.getAtghType() );
  TRACE_CODEC( "OccupancyPackingBlockSize           = %d \n", context.getOccupancyPackingBlockSize() );
  size_t totalNumberOfMps = 0;
  size_t patchIndex       = 0;
  int32_t packingBlockSize = context.getOccupancyPackingBlockSize();
  int32_t quantizerSizeX = 1<<atgh.getAtghPatchSizeXinfoQuantizer();
  int32_t quantizerSizeY = 1<<atgh.getAtghPatchSizeYinfoQuantizer();
  frame.setLog2PatchQuantizerSizeX( atgh.getAtghPatchSizeXinfoQuantizer () );
  frame.setLog2PatchQuantizerSizeY( atgh.getAtghPatchSizeYinfoQuantizer () );
  for ( patchIndex = 0; patchIndex < patchCount; patchIndex++ ) {
    auto&        pid           = atgdu.getPatchInformationData( patchIndex );
    PCCPatchType currPatchType = getCurrPatchType( tileGroupType, atgdu.getPatchMode( patchIndex ) );
    if ( currPatchType == INTRA_PATCH ) {
      auto& patch                    = patches[patchIndex];
      patch.getOccupancyResolution() = context.getOccupancyPackingBlockSize();
      auto& pdu                      = pid.getPatchDataUnit();
      patch.getU0()                  = pdu.getPdu2dPosX();
      patch.getV0()                  = pdu.getPdu2dPosY();
      patch.getU1()                  = pdu.getPdu3dPosX();
      patch.getV1()                  = pdu.getPdu3dPosY();

      bool lodEnableFlag = pdu.getLodEnableFlag();
      if ( lodEnableFlag ) {
        patch.setLodScaleX( pdu.getLodScaleXminus1() + 1 );
        patch.setLodScaleY( pdu.getLodScaleY() + ( patch.getLodScaleX() > 1 ? 1 : 2 ) );
      } else {
        patch.setLodScaleX( 1 );
        patch.setLodScaleY( 1 );
      }
      patch.getSizeD()  = ( std::min )( pdu.getPdu3dPosDeltaMaxZ() * minLevel, (size_t)255 );
      if(asps.getPatchSizeQuantizerPresentFlag()){
        int32_t quantizedDeltaSizeU = pdu.getPdu2dDeltaSizeX();
        int32_t quantizedDeltaSizeV = pdu.getPdu2dDeltaSizeY();
        patch.setPatchSize2DXInPixel( prevPatchSize2DXInPixel + quantizedDeltaSizeU*quantizerSizeX );
        patch.setPatchSize2DYInPixel( prevPatchSize2DYInPixel + quantizedDeltaSizeV*quantizerSizeY );
        patch.getSizeU0()              = ceil( (double)patch.getPatchSize2DXInPixel()/ (double)packingBlockSize);
        patch.getSizeV0()              = ceil( (double)patch.getPatchSize2DYInPixel()/ (double)packingBlockSize);
      }
      else{
        patch.getSizeU0() = prevSizeU0 + pdu.getPdu2dDeltaSizeX();
        patch.getSizeV0() = prevSizeV0 + pdu.getPdu2dDeltaSizeY();
      }
      size_t pduProjectionPlane =
          asps.get45DegreeProjectionPatchPresentFlag() ? ( pdu.getPduProjectionId() >> 2 ) : pdu.getPduProjectionId();
      size_t pdu45degreeProjectionRotationAxis =
          asps.get45DegreeProjectionPatchPresentFlag() ? ( pdu.getPduProjectionId() & 0x03 ) : 0;
      patch.getNormalAxis()            = pduProjectionPlane % 3;
      patch.getProjectionMode()        = pduProjectionPlane < 3 ? 0 : 1;
      patch.getPatchOrientation()      = pdu.getPduOrientationIndex();
      patch.getAxisOfAdditionalPlane() = pdu45degreeProjectionRotationAxis; 
      TRACE_CODEC( "patch %lu / %lu: Intra \n", patchIndex, patchCount );
      const size_t max3DCoordinate = 1 << ( gi.getGeometry3dCoordinatesBitdepthMinus1() + 1 );
      if ( patch.getProjectionMode() == 0 ) {
        patch.getD1() = (int32_t)pdu.getPdu3dPosMinZ() * minLevel;
      } else {
        if ( asps.get45DegreeProjectionPatchPresentFlag() == 0 ) {
          patch.getD1() = max3DCoordinate - (int32_t)pdu.getPdu3dPosMinZ() * minLevel;
        } else {
          patch.getD1() = ( max3DCoordinate << 1 ) - (int32_t)pdu.getPdu3dPosMinZ() * minLevel;
        }
      }
      prevSizeU0 = patch.getSizeU0();
      prevSizeV0 = patch.getSizeV0();
      prevPatchSize2DXInPixel = patch.getPatchSize2DXInPixel();
      prevPatchSize2DYInPixel = patch.getPatchSize2DYInPixel();
      if ( patch.getNormalAxis() == 0 ) {
        patch.getTangentAxis()   = 2;
        patch.getBitangentAxis() = 1;
      } else if ( patch.getNormalAxis() == 1 ) {
        patch.getTangentAxis()   = 2;
        patch.getBitangentAxis() = 0;
      } else {
        patch.getTangentAxis()   = 0;
        patch.getBitangentAxis() = 1;
      }
      TRACE_CODEC(
          "patch(Intra) %zu: UV0 %4lu %4lu UV1 %4lu %4lu D1=%4lu S=%4lu %4lu %4lu(%4lu) P=%lu O=%lu A=%u%u%u Lod "
          "=(%zu) %lu,%lu 45=%d ProjId=%4lu Axis=%lu \n",
          patchIndex, patch.getU0(), patch.getV0(), patch.getU1(), patch.getV1(), patch.getD1(), patch.getSizeU0(),
          patch.getSizeV0(), patch.getSizeD(), pdu.getPdu3dPosDeltaMaxZ(), patch.getProjectionMode(),
          patch.getPatchOrientation(), patch.getNormalAxis(), patch.getTangentAxis(), patch.getBitangentAxis(),
          (size_t)lodEnableFlag, patch.getLodScaleX(), patch.getLodScaleY(), asps.get45DegreeProjectionPatchPresentFlag(),
          pdu.getPduProjectionId(), patch.getAxisOfAdditionalPlane() );
      patch.allocOneLayerData();
      if ( asps.getPointLocalReconstructionEnabledFlag() ) {
        setPointLocalReconstructionData( frame, patch, pdu.getPointLocalReconstructionData(),
                                         context.getOccupancyPackingBlockSize() );
      }
    }
    else if(currPatchType == INTER_PATCH) {
      auto& patch                    = patches[patchIndex];
      patch.getOccupancyResolution() = context.getOccupancyPackingBlockSize();
      auto& ipdu                     = pid.getInterPatchDataUnit();

      TRACE_CODEC( "patch %lu / %lu: Inter \n", patchIndex, patchCount );
      TRACE_CODEC(
          "IPDU: refAtlasFrame= %d refPatchIdx = %d pos2DXY = %ld %ld pos3DXYZW = %ld %ld %ld %ld size2D = %ld %ld \n",
          ipdu.getIpduRefIndex(), ipdu.getIpduRefPatchIndex(), ipdu.getIpdu2dPosX(), ipdu.getIpdu2dPosY(),
          ipdu.getIpdu3dPosX(), ipdu.getIpdu3dPosY(), ipdu.getIpdu3dPosMinZ(), ipdu.getIpdu3dPosDeltaMaxZ(),
          ipdu.getIpdu2dDeltaSizeX(), ipdu.getIpdu2dDeltaSizeY() );
      patch.setBestMatchIdx( ( int32_t )( ipdu.getIpduRefPatchIndex() + predIndex ) );
      predIndex += ipdu.getIpduRefPatchIndex() + 1;
      patch.setRefAtlasFrameIndex(ipdu.getIpduRefIndex());
      size_t refPOC = frame.getRefAFOC(patch.getRefAtlasFrameIndex());
      const auto& refPatch = context.getFrame(refPOC).getPatches()[patch.getBestMatchIdx()];
      TRACE_CODEC( "\trefPatch: Idx = %lu UV0 = %lu %lu  UV1 = %lu %lu Size = %lu %lu %lu  Lod = %u,%u\n",
                   patch.getBestMatchIdx(), refPatch.getU0(), refPatch.getV0(), refPatch.getU1(), refPatch.getV1(),
                   refPatch.getSizeU0(), refPatch.getSizeV0(), refPatch.getSizeD(), refPatch.getLodScaleX(),
                   refPatch.getLodScaleY() );
      patch.getProjectionMode()        = refPatch.getProjectionMode();
      patch.getU0()                    = ipdu.getIpdu2dPosX() + refPatch.getU0();
      patch.getV0()                    = ipdu.getIpdu2dPosY() + refPatch.getV0();
      patch.getPatchOrientation()      = refPatch.getPatchOrientation();
      patch.getU1()                    = ipdu.getIpdu3dPosX() + refPatch.getU1();
      patch.getV1()                    = ipdu.getIpdu3dPosY() + refPatch.getV1();
      if(asps.getPatchSizeQuantizerPresentFlag()){
        patch.setPatchSize2DXInPixel( refPatch.getPatchSize2DXInPixel() + (ipdu.getIpdu2dDeltaSizeX())*quantizerSizeX );
        patch.setPatchSize2DYInPixel(refPatch.getPatchSize2DYInPixel() + (ipdu.getIpdu2dDeltaSizeY())*quantizerSizeY );        
        patch.getSizeU0()              = ceil( (double)patch.getPatchSize2DXInPixel()/ (double)packingBlockSize);
        patch.getSizeV0()              = ceil( (double)patch.getPatchSize2DYInPixel()/ (double)packingBlockSize);
      } else {
        patch.getSizeU0()                = ipdu.getIpdu2dDeltaSizeX() + refPatch.getSizeU0();
        patch.getSizeV0()                = ipdu.getIpdu2dDeltaSizeY() + refPatch.getSizeV0();
      }
      patch.getNormalAxis()            = refPatch.getNormalAxis();
      patch.getTangentAxis()           = refPatch.getTangentAxis();
      patch.getBitangentAxis()         = refPatch.getBitangentAxis();
      patch.getAxisOfAdditionalPlane() = refPatch.getAxisOfAdditionalPlane();
      const size_t max3DCoordinate     = 1 << ( gi.getGeometry3dCoordinatesBitdepthMinus1() + 1 );
      if ( patch.getProjectionMode() == 0 ) {
        patch.getD1() = ( ipdu.getIpdu3dPosMinZ() + ( refPatch.getD1() / minLevel ) ) * minLevel;
      } else {
        if ( asps.get45DegreeProjectionPatchPresentFlag() == 0 ) {
          patch.getD1() =
              max3DCoordinate -
              ( ipdu.getIpdu3dPosMinZ() + ( ( max3DCoordinate - refPatch.getD1() ) / minLevel ) ) * minLevel;
        } else {
          patch.getD1() =
              ( max3DCoordinate << 1 ) -
              ( ipdu.getIpdu3dPosMinZ() + ( ( ( max3DCoordinate << 1 ) - refPatch.getD1() ) / minLevel ) ) * minLevel;
        }
      }
      const int64_t delta_DD = ipdu.getIpdu3dPosDeltaMaxZ();
      size_t        prevDD   = refPatch.getSizeD() / minLevel;
      if ( prevDD * minLevel != refPatch.getSizeD() ) { prevDD += 1; }
      patch.getSizeD() = ( std::min )( size_t( ( delta_DD + prevDD ) * minLevel ), (size_t)255 );
      patch.setLodScaleX( refPatch.getLodScaleX() );
      patch.setLodScaleY( refPatch.getLodScaleY() );
      prevSizeU0 = patch.getSizeU0();
      prevSizeV0 = patch.getSizeV0();
      prevPatchSize2DXInPixel       = patch.getPatchSize2DXInPixel();
      prevPatchSize2DYInPixel       = patch.getPatchSize2DYInPixel();

      TRACE_CODEC(
          "patch(Inter) %zu: UV0 %4lu %4lu UV1 %4lu %4lu D1=%4lu S=%4lu %4lu %4lu from DeltaSize = "
          "%4ld %4ld P=%lu O=%lu A=%u%u%u Lod = %lu,%lu \n",
          patchIndex, patch.getU0(), patch.getV0(), patch.getU1(), patch.getV1(), patch.getD1(), patch.getSizeU0(),
          patch.getSizeV0(), patch.getSizeD(), ipdu.getIpdu2dDeltaSizeX(), ipdu.getIpdu2dDeltaSizeY(),
          patch.getProjectionMode(), patch.getPatchOrientation(), patch.getNormalAxis(), patch.getTangentAxis(),
          patch.getBitangentAxis(), patch.getLodScaleX(), patch.getLodScaleY() );

      patch.allocOneLayerData(); //do we need this?
      if ( asps.getPointLocalReconstructionEnabledFlag() ) {
        setPointLocalReconstructionData( frame, patch, ipdu.getPointLocalReconstructionData(),
                                         context.getOccupancyPackingBlockSize() );
      }
    }
    else if(currPatchType == MERGE_PATCH) {
      assert(-2);
      auto& patch                    = patches[patchIndex];
      patch.getOccupancyResolution() = context.getOccupancyPackingBlockSize();
      auto& mpdu                     = pid.getMergePatchDataUnit();
      bool overridePlrFlag=false;
      const size_t max3DCoordinate     = 1 << ( gi.getGeometry3dCoordinatesBitdepthMinus1() + 1 );

      TRACE_CODEC( "patch %lu / %lu: Inter \n", patchIndex, patchCount );
      TRACE_CODEC(
                  "MPDU: refAtlasFrame= %d refPatchIdx = ?? pos2DXY = %ld %ld pos3DXYZW = %ld %ld %ld %ld size2D = %ld %ld \n",
                  mpdu.getMpduRefIndex(),    mpdu.getMpdu2dPosX(), mpdu.getMpdu2dPosY(),
                  mpdu.getMpdu3dPosX(),   mpdu.getMpdu3dPosY(), mpdu.getMpdu3dPosMinZ(), mpdu.getMpdu3dPosDeltaMaxZ(),
                  mpdu.getMpdu2dDeltaSizeX(), mpdu.getMpdu2dDeltaSizeY() );
      
//      patch.setBestMatchIdx( ( int32_t )( mpdu.getMpduRefPatchIndex() + predIndex ) );
//      predIndex += mpdu.getMpduRefPatchIndex() + 1;
      patch.setBestMatchIdx( patchIndex );
      predIndex = patchIndex;
      patch.setRefAtlasFrameIndex(mpdu.getMpduRefIndex());
      size_t refPOC = frame.getRefAFOC(patch.getRefAtlasFrameIndex());
      const auto& refPatch = context.getFrame(refPOC).getPatches()[patch.getBestMatchIdx()];
      
      if( mpdu.getMpduOverride2dParamsFlag() ){
        patch.getU0()                    = mpdu.getMpdu2dPosX() + refPatch.getU0();
        patch.getV0()                    = mpdu.getMpdu2dPosY() + refPatch.getV0();
        if(asps.getPatchSizeQuantizerPresentFlag()){
          patch.setPatchSize2DXInPixel( refPatch.getPatchSize2DXInPixel() + (mpdu.getMpdu2dDeltaSizeX())*quantizerSizeX );
          patch.setPatchSize2DYInPixel(refPatch.getPatchSize2DYInPixel() + (mpdu.getMpdu2dDeltaSizeY())*quantizerSizeY );
          
          patch.getSizeU0()              = ceil( (double)patch.getPatchSize2DXInPixel()/ (double)packingBlockSize);
          patch.getSizeV0()              = ceil( (double)patch.getPatchSize2DYInPixel()/ (double)packingBlockSize);
        } else {
          patch.getSizeU0()                = mpdu.getMpdu2dDeltaSizeX() + refPatch.getSizeU0();
          patch.getSizeV0()                = mpdu.getMpdu2dDeltaSizeY() + refPatch.getSizeV0();
        }
        
        if( asps.getPointLocalReconstructionEnabledFlag()) overridePlrFlag=true;
      }
      else{
        
        if(mpdu.getMpduOverride3dParamsFlag()) {
          patch.getU1()                    = mpdu.getMpdu3dPosX() + refPatch.getU1();
          patch.getV1()                    = mpdu.getMpdu3dPosY() + refPatch.getV1();
          if ( patch.getProjectionMode() == 0 ) {
            patch.getD1() = ( mpdu.getMpdu3dPosMinZ() + ( refPatch.getD1() / minLevel ) ) * minLevel;
          } else {
            if ( asps.get45DegreeProjectionPatchPresentFlag() == 0 ) {
              patch.getD1() = max3DCoordinate -
              ( mpdu.getMpdu3dPosMinZ() + ( ( max3DCoordinate - refPatch.getD1() ) / minLevel ) ) * minLevel;
            } else {
              patch.getD1() = ( max3DCoordinate << 1 ) -
              ( mpdu.getMpdu3dPosMinZ() + ( ( ( max3DCoordinate << 1 ) - refPatch.getD1() ) / minLevel ) ) * minLevel;
            }
          }
          
          const int64_t delta_DD = mpdu.getMpdu3dPosDeltaMaxZ();
          size_t        prevDD   = refPatch.getSizeD() / minLevel;
          if ( prevDD * minLevel != refPatch.getSizeD() ) { prevDD += 1; }
          patch.getSizeD() = ( std::min )( size_t( ( delta_DD + prevDD ) * minLevel ), (size_t)255 );

          if( asps.getPointLocalReconstructionEnabledFlag())  overridePlrFlag=mpdu.getMpduOverridePlrFlag();
        }

        
      }
      

      patch.getProjectionMode()        = refPatch.getProjectionMode();
      patch.getPatchOrientation()      = refPatch.getPatchOrientation();

      patch.getNormalAxis()            = refPatch.getNormalAxis();
      patch.getTangentAxis()           = refPatch.getTangentAxis();
      patch.getBitangentAxis()         = refPatch.getBitangentAxis();
      patch.getAxisOfAdditionalPlane() = refPatch.getAxisOfAdditionalPlane();
      


      patch.setLodScaleX( refPatch.getLodScaleX() );
      patch.setLodScaleY( refPatch.getLodScaleY() );
      prevSizeU0 = patch.getSizeU0();
      prevSizeV0 = patch.getSizeV0();
      prevPatchSize2DXInPixel       = patch.getPatchSize2DXInPixel();
      prevPatchSize2DYInPixel       = patch.getPatchSize2DYInPixel();
      
      TRACE_CODEC(
                  "patch(Inter) %zu: UV0 %4lu %4lu UV1 %4lu %4lu D1=%4lu S=%4lu %4lu %4lu from DeltaSize = "
                  "%4ld %4ld P=%lu O=%lu A=%u%u%u Lod = %lu,%lu \n",
                  patchIndex, patch.getU0(), patch.getV0(), patch.getU1(), patch.getV1(), patch.getD1(), patch.getSizeU0(),
                  patch.getSizeV0(), patch.getSizeD(), mpdu.getMpdu2dDeltaSizeX(), mpdu.getMpdu2dDeltaSizeY(),
                  patch.getProjectionMode(), patch.getPatchOrientation(), patch.getNormalAxis(), patch.getTangentAxis(),
                  patch.getBitangentAxis(), patch.getLodScaleX(), patch.getLodScaleY() );
      
      patch.allocOneLayerData(); //do we need this?
      if ( asps.getPointLocalReconstructionEnabledFlag() ) {
        setPointLocalReconstructionData( frame, patch, mpdu.getPointLocalReconstructionData(),
                                        context.getOccupancyPackingBlockSize() );
      }
    }
    else if(currPatchType == SKIP_PATCH){
      assert(-1);
      auto& patch                    = patches[patchIndex];
      TRACE_CODEC( "patch %lu / %lu: Inter \n", patchIndex, patchCount );
      TRACE_CODEC( "SDU: refAtlasFrame= 0 refPatchIdx = %d \n", patchIndex );
      
      patch.setBestMatchIdx( ( int32_t )( patchIndex ) );
      predIndex += patchIndex;
      patch.setRefAtlasFrameIndex(0);
      size_t refPOC = frame.getRefAFOC(patch.getRefAtlasFrameIndex());
      const auto& refPatch = context.getFrame(refPOC).getPatches()[patch.getBestMatchIdx()];
      TRACE_CODEC( "\trefPatch: Idx = %lu UV0 = %lu %lu  UV1 = %lu %lu Size = %lu %lu %lu  Lod = %u,%u\n",
                  patch.getBestMatchIdx(), refPatch.getU0(), refPatch.getV0(), refPatch.getU1(), refPatch.getV1(),
                  refPatch.getSizeU0(), refPatch.getSizeV0(), refPatch.getSizeD(), refPatch.getLodScaleX(),
                  refPatch.getLodScaleY() );
      
      patch.getProjectionMode()        = refPatch.getProjectionMode();
      patch.getU0()                    = refPatch.getU0();
      patch.getV0()                    = refPatch.getV0();
      patch.getPatchOrientation()      = refPatch.getPatchOrientation();
      patch.getU1()                    = refPatch.getU1();
      patch.getV1()                    = refPatch.getV1();
      if(asps.getPatchSizeQuantizerPresentFlag()){
        patch.setPatchSize2DXInPixel( refPatch.getPatchSize2DXInPixel());
        patch.setPatchSize2DYInPixel(refPatch.getPatchSize2DYInPixel());
        
        patch.getSizeU0()              = ceil( (double)patch.getPatchSize2DXInPixel()/ (double)packingBlockSize);
        patch.getSizeV0()              = ceil( (double)patch.getPatchSize2DYInPixel()/ (double)packingBlockSize);
      } else {
        patch.getSizeU0()                = refPatch.getSizeU0();
        patch.getSizeV0()                = refPatch.getSizeV0();
      }
      patch.getNormalAxis()            = refPatch.getNormalAxis();
      patch.getTangentAxis()           = refPatch.getTangentAxis();
      patch.getBitangentAxis()         = refPatch.getBitangentAxis();
      patch.getAxisOfAdditionalPlane() = refPatch.getAxisOfAdditionalPlane();
      const size_t max3DCoordinate     = 1 << ( gi.getGeometry3dCoordinatesBitdepthMinus1() + 1 );
      if ( patch.getProjectionMode() == 0 ) {
        patch.getD1() = ( ( refPatch.getD1() / minLevel ) ) * minLevel;
      } else {
        if ( asps.get45DegreeProjectionPatchPresentFlag() == 0 ) {
          patch.getD1() =
          max3DCoordinate -
          ( ( ( max3DCoordinate - refPatch.getD1() ) / minLevel ) ) * minLevel;
        } else {
          patch.getD1() =
          ( max3DCoordinate << 1 ) -
          ( ( ( ( max3DCoordinate << 1 ) - refPatch.getD1() ) / minLevel ) ) * minLevel;
        }
      }      
      size_t        prevDD   = refPatch.getSizeD() / minLevel;
      if ( prevDD * minLevel != refPatch.getSizeD() ) { prevDD += 1; }
      patch.getSizeD() = ( std::min )( size_t( ( prevDD ) * minLevel ), (size_t)255 );
      patch.setLodScaleX( refPatch.getLodScaleX() );
      patch.setLodScaleY( refPatch.getLodScaleY() );
      prevSizeU0 = patch.getSizeU0();
      prevSizeV0 = patch.getSizeV0();
      prevPatchSize2DXInPixel       = patch.getPatchSize2DXInPixel();
      prevPatchSize2DYInPixel       = patch.getPatchSize2DYInPixel();
      TRACE_CODEC(
                  "patch(skip) %zu: UV0 %4lu %4lu UV1 %4lu %4lu D1=%4lu S=%4lu %4lu %4lu P=%lu O=%lu A=%u%u%u Lod = %lu,%lu \n",
                  patchIndex, patch.getU0(), patch.getV0(), patch.getU1(), patch.getV1(), patch.getD1(), patch.getSizeU0(),
                  patch.getSizeV0(), patch.getSizeD(),
                  patch.getProjectionMode(), patch.getPatchOrientation(), patch.getNormalAxis(), patch.getTangentAxis(),
                  patch.getBitangentAxis(), patch.getLodScaleX(), patch.getLodScaleY() );
      patch.allocOneLayerData(); 
    }
    else if(currPatchType == RAW_PATCH) {
      TRACE_CODEC( "patch %lu / %lu: raw \n", patchIndex, patchCount );

      auto& ppdu                = pid.getRawPatchDataUnit();
      auto& missedPointsPatch   = pcmPatches[patchIndex - numNonRawPatch];
      missedPointsPatch.u0_     = ppdu.getRpdu2dPosX();
      missedPointsPatch.v0_     = ppdu.getRpdu2dPosY();
      missedPointsPatch.sizeU0_ = ppdu.getRpdu2dDeltaSizeX();
      missedPointsPatch.sizeV0_ = ppdu.getRpdu2dDeltaSizeY();
      if ( afps.getAfpsRaw3dPosBitCountExplicitModeFlag() ) {
        missedPointsPatch.u1_ = ppdu.getRpdu3dPosX();
        missedPointsPatch.v1_ = ppdu.getRpdu3dPosY();
        missedPointsPatch.d1_ = ppdu.getRpdu3dPosZ();
      } else {
        const size_t pcmU1V1D1Level = 1 << ( gi.getGeometryNominal2dBitdepthMinus1() + 1 );
        missedPointsPatch.u1_       = ppdu.getRpdu3dPosX() * pcmU1V1D1Level;
        missedPointsPatch.v1_       = ppdu.getRpdu3dPosY() * pcmU1V1D1Level;
        missedPointsPatch.d1_       = ppdu.getRpdu3dPosZ() * pcmU1V1D1Level;
      }
      missedPointsPatch.setNumberOfMps( ppdu.getRpduRawPoints() );
      missedPointsPatch.occupancyResolution_ = context.getOccupancyPackingBlockSize();
      totalNumberOfMps += missedPointsPatch.getNumberOfMps();
      TRACE_CODEC( "Raw :UV = %lu %lu  size = %lu %lu  uvd1 = %lu %lu %lu numPoints = %lu ocmRes = %lu \n",
                   missedPointsPatch.u0_, missedPointsPatch.v0_, missedPointsPatch.sizeU0_, missedPointsPatch.sizeV0_,
                   missedPointsPatch.u1_, missedPointsPatch.v1_, missedPointsPatch.d1_, missedPointsPatch.numberOfMps_,
                   missedPointsPatch.occupancyResolution_ );
    }
      else if(currPatchType == EOM_PATCH) {
      TRACE_CODEC( "patch %lu / %lu: EOM \n", patchIndex, patchCount );
      auto&       epdu       = pid.getEomPatchDataUnit();
      auto&       eomPatches = frame.getEomPatches();
      PCCEomPatch eomPatch;
      eomPatch.u0_    = epdu.getEpdu2dPosX();
      eomPatch.v0_    = epdu.getEpdu2dPosY();
      eomPatch.sizeU_ = epdu.getEpdu2dDeltaSizeX();
      eomPatch.sizeV_ = epdu.getEpdu2dDeltaSizeY();
      eomPatch.memberPatches.resize( epdu.getEpduAssociatedPatchesCountMinus1() + 1 );
      eomPatch.eddCountPerPatch.resize( epdu.getEpduAssociatedPatchesCountMinus1() + 1 );
      eomPatch.eddCount_ = 0;
      for ( size_t i = 0; i < eomPatch.memberPatches.size(); i++ ) {
        eomPatch.memberPatches[i]    = epdu.getEpduAssociatedPatches( i );
        eomPatch.eddCountPerPatch[i] = epdu.getEpduEomPointsPerPatch( i );
        eomPatch.eddCount_ += eomPatch.eddCountPerPatch[i];
      }
      eomPatches.push_back( eomPatch );
      TRACE_CODEC( "EOM: U0V0 %lu,%lu\tSizeU0V0 %lu,%lu\tN= %lu,%lu\n", eomPatch.u0_, eomPatch.v0_, eomPatch.sizeU_,
                   eomPatch.sizeV_, eomPatch.memberPatches.size(), eomPatch.eddCount_ );
      for ( size_t i = 0; i < eomPatch.memberPatches.size(); i++ ) {
        TRACE_CODEC( "%lu, %lu\n", eomPatch.memberPatches[i], eomPatch.eddCountPerPatch[i] );
      }
      TRACE_CODEC( "\n" );

    } else if(currPatchType == END_PATCH) {
      break;
    } else {
      printf( "Error: unknow frame/patch type \n" );
      TRACE_CODEC( "Error: unknow frame/patch type \n" );
    }
  }
  TRACE_CODEC( "patch %lu / %lu: end \n", patches.size(), patches.size() );
  frame.setTotalNumberOfMissedPoints( totalNumberOfMps );
}
