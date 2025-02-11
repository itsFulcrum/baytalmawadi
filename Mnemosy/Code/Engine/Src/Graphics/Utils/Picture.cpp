#include "Include/Graphics/Utils/Picture.h"

#include "Include/MnemosyConfig.h"
#include "Include/Core/Log.h"

#include "Include/MnemosyEngine.h"
#include "Include/Core/Clock.h"

// std
#include <filesystem>
#include <fstream>
#include <math.h>
#include <stdint.h>
#include <vector>
#include <memory>

// SIMD SSE/AVX
//#define PICTURE_DISABLE_SIMD

#include <immintrin.h>


// Png - lodepng
#include <lodepng/lodepng.h>

// Tiff - libtiff
#include "tiffio.h"

// Exr - openExr
#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <ImfRgbaFile.h>
#include <ImfArray.h>
#include <ImfChannelList.h>
#include <ImfMatrixAttribute.h>
#include <ImfStringAttribute.h>
#include <ImfMatrixAttribute.h>
#include <ImfStringAttribute.h>
#include <ImfEnvmapAttribute.h>
#include <ImfFrameBuffer.h>

// jpg, hdr - stbImage
//#define STBI_NO_JPEG
//#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
//#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// libktx
#include "Include/Graphics/Utils/KtxImage.h"
#include <ktx.h>
#include <ktxint.h>
#include <vkformat_enum.h>


namespace mnemosy::graphics {

	void TiffReadStripsPerThread(TIFF* handle,void* buffer,graphics::TextureFormat channelFormat,uint32_t stripSize, uint32_t stripStart,uint32_t stripEnd ) {


		if (channelFormat == TextureFormat::MNSY_R8) {

			for (uint32_t strip = stripStart; strip < stripEnd; strip++) {
				size_t offset = strip * stripSize;
				TIFFReadEncodedStrip(handle, strip, (uint8_t*)buffer + offset, -1);
			}
		}
		else if (channelFormat == TextureFormat::MNSY_R16) {

			uint32_t stripOffset = stripSize / sizeof(uint16_t);

			for (uint32_t strip = stripStart; strip < stripEnd; strip++) {
				size_t offset = strip * stripOffset;
				TIFFReadEncodedStrip(handle, strip, (uint16_t*)buffer + offset, -1);
			}
		}
		else if (channelFormat == TextureFormat::MNSY_R32) {

			uint32_t stripOffset = stripSize / sizeof(uint32_t);

			for (uint32_t strip = stripStart; strip < stripEnd; strip++) {
				size_t offset = strip * stripOffset;
				TIFFReadEncodedStrip(handle, strip, (uint32_t*)buffer + offset, -1);
			}
		}
	}

	void pic_util_SwapEndianness(unsigned char* buf, uint32_t bufStart, uint32_t bufEnd) {


#ifndef PICTURE_DISABLE_SIMD


		// Process 16 bytes (8 pixels) at a time using SSE2
		const uint32_t pixelsPerIteration = 8;
		const uint32_t bytesPerIteration = pixelsPerIteration * 2;

		// Start pointer
		unsigned char* start = buf + bufStart * 2;
		unsigned char* end = buf + bufEnd * 2;

		// Loop through the buffer in chunks of 16 bytes
		for (; start + bytesPerIteration <= end; start += bytesPerIteration) {
			// Load 16 bytes (8 pixels) into an SSE2 register
			__m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(start));

			// Swap adjacent bytes in the 16-byte chunk
			__m128i swapped = _mm_or_si128(
				_mm_slli_epi16(data, 8), // Shift left by 8 bits
				_mm_srli_epi16(data, 8) // Shift right by 8 bits
			);

			// Store the swapped data back to memory
			_mm_storeu_si128(reinterpret_cast<__m128i*>(start), swapped);
		}

		// Handle remaining pixels (less than 8 pixels) with scalar code
		for (; start < end; start += 2) {
			std::swap(start[0], start[1]);
		}
#else

		for (size_t i = bufStart; i < bufEnd; i++) {
			// The pointer to the current 16-bit pixel
			unsigned char* pixel = buf + i * 2;
			// Swap the two bytes to convert from big-endian to little-endian
			unsigned char temp = pixel[0];
			pixel[0] = pixel[1];
			pixel[1] = temp;
		}

#endif // PICTURE_USE_SIMD

	}


	void pic_util_linear2srgb_floatBuffer(float* buffer, size_t pixelCount) {

#ifndef PICTURE_DISABLE_SIMD

		const __m256 threshold = _mm256_set1_ps(0.0031308f);
		const __m256 multiplier = _mm256_set1_ps(12.92f);
		const __m256 a = _mm256_set1_ps(1.055f);
		const __m256 b = _mm256_set1_ps(0.055f);
		const __m256 exponent = _mm256_set1_ps(1.0f / 2.4f);

		size_t simdWidth = 8; // AVX processes 8 floats at a time
		size_t simdCount = pixelCount / simdWidth;

		for (size_t i = 0; i < simdCount; ++i) {
			// Load 8 floats from the buffer
			__m256 linearValue = _mm256_loadu_ps(buffer + i * simdWidth);

			// Perform the linear-to-sRGB transformation
			__m256 mask = _mm256_cmp_ps(linearValue, threshold, _CMP_LE_OS); // linearValue <= 0.0031308f
			__m256 lowerPart = _mm256_mul_ps(linearValue, multiplier);       // linearValue * 12.92f

			// Compute pow(linearValue, 1.0 / 2.4)
			__m256 upperPart = _mm256_sub_ps(
				_mm256_mul_ps(a, _mm256_pow_ps(linearValue, exponent)), b);

			// Select between lowerPart and upperPart based on the mask
			__m256 result = _mm256_blendv_ps(upperPart, lowerPart, mask);

			// Store the result back to the buffer
			_mm256_storeu_ps(buffer + i * simdWidth, result);
		}

		// Handle any remaining pixels that don't fit into an AVX register
		size_t remainingStart = simdCount * simdWidth;
		for (size_t i = remainingStart; i < pixelCount; ++i) {
			float val = buffer[i];
			if (val <= 0.0031308f) {
				buffer[i] = val * 12.92f;
			}
			else {
				buffer[i] = 1.055f * std::pow(val, 1.0f / 2.4f) - 0.055f;
			}
		}
#else
		for (int p = 0; p < pixelCount; p++) {

				float val = buffer[p];
				buffer[p] = Picture::pic_util_linear2srgb_float(val);
		}

#endif // PICTURE_USE_SIMD

	}


	PictureInfo Picture::ReadPicture(PictureError& outPictureError, const char* filepath,const bool flipVertically, const bool convertGrayToRGB, const bool convertEXRandHDRToSrgb) {
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";

		std::filesystem::path p = { filepath };

		if (!std::filesystem::exists(p)) {
			outPictureError.wasSuccessfull = true;
			outPictureError.what = "Read: filepath does exist.";
			return PictureInfo();
		}

		ImageFileFormat fileFormat = TexUtil::get_imageFileFormat_from_fileExtentionString(p.extension().generic_string());

		if (fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_NONE) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "Read: Image File Extention '" + p.extension().generic_string() + "' is not supported.";
			return PictureInfo();
		}

		
		//bool convertGrayToRGB = PBRTypeHint == PBRTextureType::MNSY_TEXTURE_ALBEDO || PBRTypeHint == PBRTextureType::MNSY_TEXTURE_EMISSION;	


		// load the texture
		if (fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_TIF) {
			
			return Picture::ReadTiff(outPictureError,filepath,flipVertically,convertGrayToRGB);			
		}
		else if (fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_PNG) {

			return Picture::ReadPng(outPictureError, filepath, flipVertically, convertGrayToRGB);
		}
		else if (fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_JPG) {
			
			return Picture::ReadJpg(outPictureError,filepath,flipVertically, convertGrayToRGB);
		}
		else if (fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_HDR) {
			
			return Picture::ReadHdr(outPictureError,filepath, flipVertically, convertEXRandHDRToSrgb);
		}
		else if (fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_EXR) {
					
			return Picture::ReadExr(outPictureError, filepath, flipVertically, convertEXRandHDRToSrgb, convertGrayToRGB);

		}
		else if (fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_KTX2) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "Read: File format ktx2 is not yet implemented.";

			return PictureInfo();
		}

		MNEMOSY_ASSERT(false, "All ImageFileFormat types should be represented above.");

		return PictureInfo();
	}

	// same as ReadPicture but instead of returning picture info it is passed as second parameter so we can used it for multithreading when loading several images simulaniously
	void Picture::ReadPicture_PbrThreaded(PictureError& outPictureError, PictureInfo& outPicInfo, const std::string filepath, const bool flipVertically, graphics::PBRTextureType PBRTypeHint) {

		bool convertGrayToRGB = PBRTypeHint == PBRTextureType::MNSY_TEXTURE_ALBEDO || PBRTypeHint == PBRTextureType::MNSY_TEXTURE_EMISSION;

		outPicInfo = Picture::ReadPicture(outPictureError,filepath.c_str(), flipVertically,convertGrayToRGB,false);
	}
	
	void Picture::WritePicture(PictureError& outPictureError, const char* filepath, const PictureInfo& pictureInfo, const bool flipVertically,const bool convertExrAndHdrToLinear){

		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";
		
		std::filesystem::path p = {filepath};
		ImageFileFormat fileFormat = TexUtil::get_imageFileFormat_from_fileExtentionString(p.extension().generic_string());

		if(fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_NONE){
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "Write: Image File Extention '" +  p.extension().generic_string() + "' is not supported.";
			return;
		}		

		if(fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_TIF){
			Picture::WriteTiff(outPictureError, filepath,  pictureInfo, flipVertically);
			return;
		}
		else if(fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_PNG){
			Picture::WritePng(outPictureError, filepath, pictureInfo, flipVertically);
			return;
		}
		else if(fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_JPG){

			WriteJpg(outPictureError,filepath,pictureInfo,flipVertically);
			return;
		}
		else if(fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_HDR){
			
			WriteHdr(outPictureError, filepath, pictureInfo, flipVertically, convertExrAndHdrToLinear);
			return;
		}
		else if(fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_EXR){

			Picture::WriteExr(outPictureError, filepath, pictureInfo, flipVertically, convertExrAndHdrToLinear);
			return;
		}
		else if(fileFormat == ImageFileFormat::MNSY_FILE_FORMAT_KTX2){
			//outPictureError.wasSuccessfull = false;
			//outPictureError.what = "Write: File format ktx2 is not yet implemented.";

			Picture::WriteKtx2(outPictureError,filepath,pictureInfo,flipVertically);
			return;
		}

		MNEMOSY_ASSERT(false, "All ImageFileFormat should be represented above.");

	}


	PictureInfo Picture::ReadTiff(PictureError& outPictureError, const char* filepath, const bool flipVertically, const bool convertGrayToRGB)
	{
		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";

		// kill tiff warnings in release mode.
#ifdef MNEMOSY_CONFIG_RELEASE
		TIFFSetWarningHandler(NULL);
		TIFFSetErrorHandler(NULL);
#endif // MNEMOSY_CONFIG_RELEASE


		TIFF* tif = nullptr;


		// open the tiff file.
		{
			TIFFOpenOptions* opts = TIFFOpenOptionsAlloc();

			// make sure we can read big enough images
			tmsize_t limit = (sizeof(uint32_t) * 4 * 16384 * 16384); // sizeof(uint32_t) * channels * width * height

			TIFFOpenOptionsSetMaxSingleMemAlloc(opts, limit);
			
			tif = TIFFOpenExt(filepath, "r", opts);
			
			TIFFOpenOptionsFree(opts);

			if (tif == nullptr) {
				outPictureError.wasSuccessfull = false;
				outPictureError.what = "ReadTiff: failed to open file. corrupted?";
				return PictureInfo();
			}		
		}		

		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t channels = 0;
		uint32_t bitsPerChannel = 0;

		TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
		TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
		TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &channels);		// channels per pixel (1,2,3,4)
		TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bitsPerChannel);	// bits per channel (8 , 16 or 32)

		// not needed
		//uint32_t sampleformat = 0;
		//TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &sampleformat);
		//uint32_t config = 0;
		//TIFFGetField(tif, TIFFTAG_PLANARCONFIG, &config);

		if (width == 0 || height == 0) {

			outPictureError.wasSuccessfull = false;
			outPictureError.what = "ReadTiff: invalid Data - width or height is 0";
			TIFFClose(tif);
			return PictureInfo();
		}

		// determine the texture format
		// channel format is for conviniance alternative to bitsPerChannel it just says how many bits per channel there are R8 = 8 bits per channel
		graphics::TextureFormat format = graphics::TextureFormat::MNSY_NONE;
		TextureFormat channelFormat;// = graphics::TexUtil::get_channel_textureFormat(format);
		{
			if (bitsPerChannel == 8) {
				switch (channels)
				{
				case 1: format = graphics::MNSY_R8;		break;
				case 2: format = graphics::MNSY_RG8;	break;
				case 3: format = graphics::MNSY_RGB8;	break;
				case 4: format = graphics::MNSY_RGBA8;	break;
				}
				channelFormat = graphics::TextureFormat::MNSY_R8;
			}
			else if (bitsPerChannel == 16) {
				switch (channels)
				{
				case 1: format = graphics::MNSY_R16;	break;
				case 2: format = graphics::MNSY_RG16;	break;
				case 3: format = graphics::MNSY_RGB16;	break;
				case 4: format = graphics::MNSY_RGBA16;	break;
				}
				channelFormat = graphics::TextureFormat::MNSY_R16;
			}
			else if (bitsPerChannel == 32) {
				switch (channels)
				{
				case 1: format = graphics::MNSY_R32;	break;
				case 2: format = graphics::MNSY_RG32;	break;
				case 3: format = graphics::MNSY_RGB32;	break;
				case 4: format = graphics::MNSY_RGBA32;	break;
				}
				channelFormat = graphics::TextureFormat::MNSY_R32;
			}

			if (format == graphics::TextureFormat::MNSY_NONE) {
				
				outPictureError.wasSuccessfull = false;
				outPictureError.what = "ReadTiff: format could not be deterimined";
				TIFFClose(tif);
				return PictureInfo();
			}
		}		
		
		
		// allocate pixel buffer
		size_t bytesPerPixel = channels * (bitsPerChannel / 8);
		size_t bufferSize = width * height * bytesPerPixel;
		uint32_t numberOfStrips = TIFFNumberOfStrips(tif);
		size_t stripSize = TIFFStripSize(tif);
		void* buffer = malloc(bufferSize);
		
		tmsize_t tiffError = 0;
		{ 
			// read pixel data per strip
			if (channelFormat == TextureFormat::MNSY_R8) {

				for (uint32_t strip = 0; strip < numberOfStrips; strip++) {
					//size_t offset = strip * stripSize;

					tiffError = TIFFReadEncodedStrip(tif, strip, (uint8_t*)buffer + (strip * stripSize) , -1);
				}
			}
			else if (channelFormat == TextureFormat::MNSY_R16) {

				uint32_t stripIncrement = stripSize / sizeof(uint16_t);

				for (uint32_t strip = 0; strip < numberOfStrips; strip++) {
					//size_t offset = strip * stripOffset;
					
					tiffError = TIFFReadEncodedStrip(tif, strip, (uint16_t*)buffer + (strip * stripIncrement), -1);
				}
			}
			else if (channelFormat == TextureFormat::MNSY_R32) {

				uint32_t stripIncrement = stripSize / sizeof(uint32_t);

				for (uint32_t strip = 0; strip < numberOfStrips; strip++) {
					//size_t offset = strip * stripOffset;
					tiffError = TIFFReadEncodedStrip(tif, strip, (uint32_t*)buffer + (strip * stripIncrement), -1);
				}
			}
		}

		TIFFClose(tif);
		tif = nullptr;

		if (tiffError == -1) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "ReadTiff: failed to read Stip";
			free(buffer);
			return PictureInfo();
		}

		// if we need to convert gray channel to rgb  this branch also handles flipVertically at the same time, if its set.
		if(channels == 1 && convertGrayToRGB){
			// updateing format from single channel to RGB 
			format = (TextureFormat)((uint8_t)channelFormat + 2); 

			void* pixels = malloc(width * height * 3 * (bitsPerChannel / 8));
			size_t srcPixelBytes = bitsPerChannel / 8;


			if (channelFormat == TextureFormat::MNSY_R8) {

				for (uint16_t h = 0; h < height; h++) {

					uint16_t y = height - h - 1; // awsume we want to flip vertically
					if (!flipVertically) {
						y = h;
					}

					for (uint16_t w = 0; w < width; w++) {


						size_t offsetSrc = (y * width + w);
						size_t offsetDest = (h * width + w) * 3; // 3 bc we now have RGB channels in destination buffer

						uint8_t val = *((uint8_t*)buffer + offsetSrc);
						((uint8_t*)pixels)[offsetDest + 0] = val;
						((uint8_t*)pixels)[offsetDest + 1] = val;
						((uint8_t*)pixels)[offsetDest + 2] = val;
					}
				}

			}
			else if (channelFormat == TextureFormat::MNSY_R16) {

#ifndef PICTURE_DISABLE_SIMD
				// Some SIMD experimental goodness happening here.


				size_t rowStride = width * 3;

				for (uint16_t h = 0; h < height; h++) {



					uint16_t y = flipVertically ? (height - h - 1) : h;
					size_t srcRowOffset = y * width;
					size_t destRowOffset = h * rowStride;

					uint16_t w = 0;
					// Process pixels in blocks of 8 using SIMD (128 bits = 8 � 16-bit values)
					for (; w <= width - 8; w += 8) {
						// Load 8 16-bit grayscale values from the source buffer
						__m128i grayscale = _mm_loadu_si128((__m128i*)((uint16_t*)buffer + srcRowOffset + w));

						// Duplicate grayscale values into RGB format
						__m128i rValues = grayscale; // R values (same as grayscale)
						__m128i gValues = grayscale; // G values (same as grayscale)
						__m128i bValues = grayscale; // B values (same as grayscale)

						// Store RGB values in the destination buffer
						_mm_storeu_si128((__m128i*)((uint16_t*)pixels + destRowOffset + w * 3 + 0), rValues); // R channel
						_mm_storeu_si128((__m128i*)((uint16_t*)pixels + destRowOffset + w * 3 + 8), gValues); // G channel
						_mm_storeu_si128((__m128i*)((uint16_t*)pixels + destRowOffset + w * 3 + 16), bValues); // B channel
					}

					// Process remaining pixels
					for (; w < width; w++) {
						uint16_t val = ((uint16_t*)buffer)[srcRowOffset + w];
						size_t destOffset = destRowOffset + w * 3;
						((uint16_t*)pixels)[destOffset + 0] = val; // R
						((uint16_t*)pixels)[destOffset + 1] = val; // G
						((uint16_t*)pixels)[destOffset + 2] = val; // B
					}
				}


#else
				for (uint16_t h = 0; h < height; h++) {

					uint16_t y = height - h - 1; // awsume we want to flip vertically
					if (!flipVertically) {
						y = h;
					}

					for (uint16_t w = 0; w < width; w++) {


						size_t offsetSrc = (y * width + w);
						size_t offsetDest = (h * width + w) * 3; // 3 bc we now have RGB channels in destination buffer

						uint16_t val = *((uint16_t*)buffer + offsetSrc);
						((uint16_t*)pixels)[offsetDest + 0] = val;
						((uint16_t*)pixels)[offsetDest + 1] = val;
						((uint16_t*)pixels)[offsetDest + 2] = val;
					}
				}

#endif // !PICTURE_DISABLE_SIMD
			}
			else if(channelFormat == TextureFormat::MNSY_R32) {

				for (uint16_t h = 0; h < height; h++) {

					uint16_t y = height - h - 1; // awsume we want to flip vertically
					if (!flipVertically) {
						y = h;
					}

					for (uint16_t w = 0; w < width; w++) {


						size_t offsetSrc = (y * width + w);
						size_t offsetDest = (h * width + w) * 3; // 3 bc we now have RGB channels in destination buffer

						float val = *((float*)buffer + offsetSrc);
						((float*)pixels)[offsetDest + 0] = val;
						((float*)pixels)[offsetDest + 1] = val;
						((float*)pixels)[offsetDest + 2] = val;
					}
				}
			}

			free(buffer);
			buffer = pixels;
		}

		// handeling flip vertically here seperatly if i dont have to convertGrayToRGB because if we only want to flip vertically it is much faster to copy row by row than pixel py pixel which is required if we convert gray to RGB
		bool alreadyFilpped = channels == 1 && convertGrayToRGB && flipVertically;

		if (flipVertically && !alreadyFilpped) {
			size_t rowSize = width * bytesPerPixel;
			unsigned char* tempRowBuf = static_cast<unsigned char*>(malloc(rowSize));

			uint16_t heightHalf = height / 2;
			uint16_t heightMinusOne = height - 1;

			for (uint32_t h = 0; h < heightHalf; h++) {

				unsigned char* rowTop	 = static_cast<unsigned char*>(buffer) + h * rowSize;
				unsigned char* rowBottom = static_cast<unsigned char*>(buffer) + (heightMinusOne - h) * rowSize;

				memcpy(tempRowBuf, rowTop, rowSize); // copy top row to temp
				memcpy(rowTop, rowBottom, rowSize);  // coppy bottom to top
				memcpy(rowBottom, tempRowBuf, rowSize); // copy top from temp to bottom
			}

			free(tempRowBuf);
		}

		// fill info struct
		PictureInfo info;
		info.textureFormat = format;
		info.isHalfFloat = false;
		info.width = (uint16_t)width;
		info.height = (uint16_t)height;
		info.pixels = buffer;
		
		return info;	
	}
	
	void Picture::WriteTiff(PictureError& outPictureError, const char* filepath, const PictureInfo& pictureInfo, const bool flipVertically) {
		
		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";

		PictureError err = Picture::pic_util_check_input_pictureInfo(pictureInfo);
		if(!err.wasSuccessfull){
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteTiff: " + err.what;
			return;
		}

		TextureFormat format = pictureInfo.textureFormat;
		uint16_t height = pictureInfo.height;
		uint16_t width = pictureInfo.width;

		uint8_t channels;
		uint8_t bitsPerChannel;
		uint8_t bytesPerPixel;

		graphics::TexUtil::get_information_from_textureFormat(format, channels, bitsPerChannel, bytesPerPixel);			

		// this should make sure we are allowed to allocate large amounts of data
		tmsize_t limit = (bytesPerPixel * pictureInfo.width * pictureInfo.height);
		TIFFOpenOptions* opts = TIFFOpenOptionsAlloc();
		TIFFOpenOptionsSetMaxSingleMemAlloc(opts, limit);

		TIFF* tif = TIFFOpenExt(filepath, "w",opts);
		TIFFOpenOptionsFree(opts);

		// Set neccessary tiff tag fields
		TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, pictureInfo.width);
		TIFFSetField(tif, TIFFTAG_IMAGELENGTH, pictureInfo.height);
		TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, channels); // channels per pixel (1,2,3,4)
		TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bitsPerChannel); // bits per channel (8 , 16 or 32)

		TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 1);
		TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
		TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);

		// for grayscale images set different tag
		if (format == graphics::MNSY_R8 || format == graphics::MNSY_R16 || format == graphics::MNSY_R32) {
			TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
		}
		else {
			TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
		}	

		// if we have 32 bit it means we have floating point data which has to be explicitly set in a tiff tag.
		if (bitsPerChannel == 32) {
			
			TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
		}
		else {
			TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
		}

		// set no compression
		TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);


		uint32_t rowSize = width * channels;

		// 8 bit per channel
		if (bitsPerChannel == 8) {

			uint8_t* srcBuffer = (uint8_t*)pictureInfo.pixels;

			if (flipVertically) {
				srcBuffer += (height - 1) * rowSize;
				for (uint16_t h = 0; h < height; h++) {
					TIFFWriteScanline(tif, srcBuffer, h, 0);
					srcBuffer -= rowSize;
				}
			}
			else {
				for (uint16_t h = 0; h < height; h++) {
					TIFFWriteScanline(tif, srcBuffer, h, 0);
					srcBuffer += rowSize;
				}
			}
		}
		// 16 bit per channel
		else if (bitsPerChannel == 16) {
			
			uint16_t* srcBuffer = (uint16_t*)pictureInfo.pixels;

			if (flipVertically) {				
				srcBuffer += (height - 1) * rowSize;				
				for (uint16_t h = 0; h < height; h++) {
					TIFFWriteScanline(tif, srcBuffer, h, 0);
					srcBuffer -= rowSize;
				}
			}
			else {
				for (uint16_t h = 0; h < height; h++) {					
					TIFFWriteScanline(tif, srcBuffer, h, 0);
					srcBuffer += rowSize;
				}
			}
		}
		// 32 bit per channel
		else if (bitsPerChannel == 32) { 

			uint32_t* srcBuffer = (uint32_t*)pictureInfo.pixels;

			if (flipVertically) {
				srcBuffer += (height - 1) * rowSize;
				for (uint16_t h = 0; h < height; h++) {
					TIFFWriteScanline(tif, srcBuffer, h, 0);
					srcBuffer -= rowSize;
				}
			}
			else {
				for (uint16_t h = 0; h < height; h++) {
					TIFFWriteScanline(tif, srcBuffer, h, 0);
					srcBuffer += rowSize;
				}
			}
		}

		TIFFClose(tif); // write the file

		/*if (error == -1) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteTiff: Tiff Scanline write failed.";
		}*/

		return;
	}

	// could use some performance improvements (e.g smd)
	PictureInfo Picture::ReadExr(PictureError& outPictureError, const char* filepath, const bool flipVertically, const bool convertToSrgb, const bool convertGrayToRGB) {
	
		namespace exr = Imf;

		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";
		PictureInfo outInfo;
				
		// check first couple of bytes for a magic number that identifies a file as an openExr file
		{	
			// implementation by openExr docs: https://openexr.com/en/latest/ReadingAndWritingImageFiles.html#miscellaneous

			std::ifstream f(filepath, std::ios_base::binary);
			char b[4];
			f.read(b, sizeof(b));
			bool isExrFile = !!f && b[0] == 0x76 && b[1] == 0x2f && b[2] == 0x31 && b[3] == 0x01;
			f.close();
			if (!isExrFile) {
				outPictureError.wasSuccessfull = false;
				outPictureError.what = "ReadExr: file is not a valid .exr file";
				return outInfo;
			}
		}

		exr::InputFile file = { filepath };

		Imath::Box2i dw = file.header().dataWindow();

		uint16_t width = dw.max.x - dw.min.x + 1;
		uint16_t height = dw.max.y - dw.min.y + 1;
		uint8_t numChannels = 0;

		const exr::ChannelList& channelsList = file.header().channels();

		// R channel determines which format the other channels need to have otherwise we throw an error
		TextureFormat channel_format = TextureFormat::MNSY_NONE;

		// check R channel 
		const exr::Channel* channel_r = channelsList.findChannel("R");
		bool r_channel_exists = false;
		TextureFormat r_channel_format = TextureFormat::MNSY_NONE;

		if (channel_r) {
			r_channel_exists = true;

			if (channel_r->type == exr::PixelType::HALF) {
				r_channel_format = TextureFormat::MNSY_R16;
			}
			else if (channel_r->type == exr::PixelType::FLOAT) {
				r_channel_format = TextureFormat::MNSY_R32;
			}
			else if (channel_r->type == exr::PixelType::UINT) {
				// not supported
				r_channel_format = TextureFormat::MNSY_NONE;
			}
			channel_format = r_channel_format;
			numChannels = 1;
		}

		

		// if R channel does not exist we search for gray channels
		bool gray_channel_exists = false;
		std::string gray_channel_name = "";
		const exr::Channel* gray_channel = nullptr;
		TextureFormat gray_channel_format = TextureFormat::MNSY_NONE;
		if (!r_channel_exists) {

			// search 
			if (channelsList.findChannel("Y")) {
				gray_channel_exists = true;
				gray_channel_name = "Y";
			}
			else if (channelsList.findChannel("Z")) {
				gray_channel_exists = true;
				gray_channel_name = "Z";
			}


			if (gray_channel_exists) {
				gray_channel = channelsList.findChannel(gray_channel_name.c_str());

				if (gray_channel->type == exr::PixelType::HALF) {
					gray_channel_format = TextureFormat::MNSY_R16;
				}
				else if (gray_channel->type == exr::PixelType::FLOAT) {
					gray_channel_format = TextureFormat::MNSY_R32;
				}
				else if (gray_channel->type == exr::PixelType::UINT) {
					// not supported
					gray_channel_format = TextureFormat::MNSY_NONE;
				}

				channel_format = gray_channel_format;
			}			
		}	


		// if no r channel and no gray channel return error;
		if (!r_channel_exists && !gray_channel_exists) {

			outPictureError.wasSuccessfull = false;
			outPictureError.what = "ReadExr: the file does not contain either an R-channel or any grayscale channels - other purposes are not supported";
			return outInfo;
		}

		if (channel_format == graphics::TextureFormat::MNSY_NONE) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "ReadExr: channel type UINT is not supported";
			return outInfo;
		}

		// ==== Gray Channel =====
		// gray channel can only exist if no r channel is present
		// if gray channel we do seperate calculations here and we return from this if statement
		if (gray_channel_exists) { 
			


			if (convertGrayToRGB) {
				numChannels = 3;
			}
			else {
				numChannels = 1;
			}

			exr::FrameBuffer fb;

			exr::Array2D<Imath::half> gray_pixels_half;
			exr::Array2D<float> gray_pixels_float;


			void* buffer = nullptr;


			if (channel_format == TextureFormat::MNSY_R16) {

				gray_pixels_half.resizeErase(height, width);
				fb.insert(gray_channel_name.c_str(), exr::Slice(exr::PixelType::HALF, (char*)(&gray_pixels_half[0][0] - dw.min.x - dw.min.y * width), sizeof(gray_pixels_half[0][0]), sizeof(gray_pixels_half[0][0]) * width, 1, 1, 0.0, false, false));

				file.setFrameBuffer(fb);
				file.readPixels(dw.min.y, dw.max.y);
				
				size_t totalBufferSize = (size_t)width * (size_t)height * (uint16_t)numChannels * sizeof(uint16_t);

				// MEM Alloc
				buffer = malloc(totalBufferSize);
				

				for (uint16_t h = 0; h < height; h++) {

					for (uint16_t w = 0; w < width; w++) {

						// awsume we want to flip vertically
						uint16_t x = w;
						uint16_t y = height - h - 1;
						if (!flipVertically) {
							y = h;
						}

						if (convertToSrgb)
							gray_pixels_half[y][x] = (Imath::half)pic_util_linear2srgb_float((float)gray_pixels_half[y][x]);


						size_t offset = (h * width + w) * numChannels;

						if (!convertGrayToRGB) {
							memcpy((uint16_t*)buffer + offset, &gray_pixels_half[y][x], sizeof(uint16_t));
						}
						else { // copy the gray channel 3 times
							memcpy((uint16_t*)buffer + offset + 0, &gray_pixels_half[y][x], sizeof(uint16_t));
							memcpy((uint16_t*)buffer + offset + 1, &gray_pixels_half[y][x], sizeof(uint16_t));
							memcpy((uint16_t*)buffer + offset + 2, &gray_pixels_half[y][x], sizeof(uint16_t));
						}

					} // loop width
				}	// loop height

			}
			else if (channel_format == TextureFormat::MNSY_R32) {

				gray_pixels_float.resizeErase(height, width);
				fb.insert(gray_channel_name.c_str(), exr::Slice(exr::PixelType::FLOAT, (char*)(&gray_pixels_float[0][0] - dw.min.x - dw.min.y * width), sizeof(gray_pixels_float[0][0]) * 1, sizeof(gray_pixels_float[0][0]) * width, 1, 1, 0.0, false, false));

				file.setFrameBuffer(fb);
				file.readPixels(dw.min.y, dw.max.y);

				size_t totalBufferSize = (size_t)width * (size_t)height * (uint16_t)numChannels * sizeof(float);

				buffer = malloc(totalBufferSize);
						

				for (uint16_t h = 0; h < height; h++) {

					for (uint16_t w = 0; w < width; w++) {

						// awsume we want to flip vertically
						uint16_t x = w;
						uint16_t y = height - h - 1;
						if (!flipVertically) {
							y = h;
						}

						size_t offset = (h * width + w) * numChannels;

						if (convertToSrgb)
							gray_pixels_float[y][x] = pic_util_linear2srgb_float(gray_pixels_float[y][x]);

						if (!convertGrayToRGB) {
							memcpy((float*)buffer + offset, &gray_pixels_float[y][x], sizeof(float));
						}
						else {
							memcpy((float*)buffer + offset + 0, &gray_pixels_float[y][x], sizeof(float));
							memcpy((float*)buffer + offset + 1, &gray_pixels_float[y][x], sizeof(float));
							memcpy((float*)buffer + offset + 2, &gray_pixels_float[y][x], sizeof(float));
						}
												
					} // loop width
				} // loop height

			} 


			// fill info struct
			outInfo.width = width;
			outInfo.height = height;

			if (convertGrayToRGB) {
				outInfo.textureFormat = (TextureFormat)( (uint8_t)channel_format + 2); // grayscale images are given back as RGB
			}
			else {
				outInfo.textureFormat = channel_format; // grayscale images are given back as R16 or R32
			}
			outInfo.isHalfFloat = channel_format == MNSY_R16;
			outInfo.pixels = buffer;

			return outInfo;


		} // !gray channel Exists

		// check G channel 
		const exr::Channel* channel_g = channelsList.findChannel("G");
		bool g_channel_exists = false;
		TextureFormat g_channel_format = TextureFormat::MNSY_NONE;

		if (channel_g && r_channel_exists) {
			g_channel_exists = true;

			exr::PixelType type = channel_g->type;

			if (type == exr::PixelType::HALF) {
				g_channel_format = TextureFormat::MNSY_R16;
			}
			else if (type == exr::PixelType::FLOAT) {
				g_channel_format = TextureFormat::MNSY_R32;
			}
			else if (type == exr::PixelType::UINT) {
				outPictureError.wasSuccessfull = false;
				outPictureError.what = "ReadExr: channel format UINT is not supported";
				return outInfo;
			}
			numChannels = 2;
		}

		// check B channel 
		const exr::Channel* channel_b = channelsList.findChannel("B");
		bool b_channel_exists = false;
		TextureFormat b_channel_format = TextureFormat::MNSY_NONE;

		if (channel_b && g_channel_exists) {
			b_channel_exists = true;

			exr::PixelType type = channel_b->type;

			if (type == exr::PixelType::HALF) {
				b_channel_format = TextureFormat::MNSY_R16;
			}
			else if (type == exr::PixelType::FLOAT) {
				b_channel_format = TextureFormat::MNSY_R32;
			}
			else if (type == exr::PixelType::UINT) {
				outPictureError.wasSuccessfull = false;
				outPictureError.what = "ReadExr: channel format UINT is not supported";
				return outInfo;
			}
			numChannels = 3;
		}

		// check A channel 
		const exr::Channel* channel_a = channelsList.findChannel("A");
		bool a_channel_exists = false;
		TextureFormat a_channel_format = TextureFormat::MNSY_NONE;

		if (channel_a && b_channel_exists) {
			a_channel_exists = true;

			exr::PixelType type = channel_a->type;

			if (type == exr::PixelType::HALF) {
				a_channel_format = TextureFormat::MNSY_R16;
			}
			else if (type == exr::PixelType::FLOAT) {
				a_channel_format = TextureFormat::MNSY_R32;
			}
			else if (type == exr::PixelType::UINT) {
				outPictureError.wasSuccessfull = false;
				outPictureError.what = "ReadExr: channel format UINT is not supported";
				return outInfo;
			}
			numChannels = 4;
		}

		// we dont support loading images where differnt channels have different bit depths. except alpha which will be promoted or demoted to fit the other channels
		{
			bool throwIncompatibleFormatError = false;
			if (g_channel_exists) {
				if (g_channel_format != channel_format) {
					throwIncompatibleFormatError = true;
				}
			}
			if (b_channel_exists) {
				if (b_channel_format != channel_format) {
					throwIncompatibleFormatError = true;
				}
			}
			if (throwIncompatibleFormatError) {
				outPictureError.wasSuccessfull = false;
				outPictureError.what = "ReadExr: A channel had a different bit depth than the previous one. This is not supported";
				return outInfo;
			}
		}


		exr::FrameBuffer framebuffer;

		// only the once that are present will be filled with data
		exr::Array2D<Imath::half> r_pixels_half;
		exr::Array2D<Imath::half> g_pixels_half;
		exr::Array2D<Imath::half> b_pixels_half;
		exr::Array2D<Imath::half> a_pixels_half;
		

		exr::Array2D<float> r_pixels_float;
		exr::Array2D<float> g_pixels_float;
		exr::Array2D<float> b_pixels_float;
		exr::Array2D<float> a_pixels_float;
		
		if (channel_format == TextureFormat::MNSY_R16) {

			if (r_channel_exists) { // actually must exist reduntant check but its more tidy
				r_pixels_half.resizeErase(height, width);
				framebuffer.insert("R", exr::Slice(exr::PixelType::HALF, (char*)(&r_pixels_half[0][0] - dw.min.x - dw.min.y * width), sizeof(r_pixels_half[0][0]), sizeof(r_pixels_half[0][0]) * width, 1, 1, 0.0, false, false));
			}			
			if (g_channel_exists) {
				g_pixels_half.resizeErase(height,width);
				framebuffer.insert("G", exr::Slice(exr::PixelType::HALF, (char*)(&g_pixels_half[0][0] - dw.min.x - dw.min.y * width), sizeof(g_pixels_half[0][0]), sizeof(g_pixels_half[0][0]) * width, 1, 1, 0.0, false, false));
			}
			if (b_channel_exists) {
				b_pixels_half.resizeErase(height, width);
				framebuffer.insert("B", exr::Slice(exr::PixelType::HALF, (char*)(&b_pixels_half[0][0] - dw.min.x - dw.min.y * width), sizeof(b_pixels_half[0][0]), sizeof(b_pixels_half[0][0]) * width, 1, 1, 0.0, false, false));
			}
		}
		else if (channel_format == TextureFormat::MNSY_R32) {

			if (r_channel_exists) {
				r_pixels_float.resizeErase(height, width);
				framebuffer.insert("R", exr::Slice(exr::PixelType::FLOAT, (char*)(&r_pixels_float[0][0] - dw.min.x - dw.min.y * width), sizeof(r_pixels_float[0][0]) * 1, sizeof(r_pixels_float[0][0])* width, 1, 1, 0.0, false, false));
			}
			if (g_channel_exists) {
				g_pixels_float.resizeErase(height, width);
				framebuffer.insert("G", exr::Slice(exr::PixelType::FLOAT, (char*)(&g_pixels_float[0][0] - dw.min.x - dw.min.y * width), sizeof(g_pixels_float[0][0]) * 1, sizeof(g_pixels_float[0][0]) * width, 1, 1, 0.0, false, false));
			}
			if (b_channel_exists) {
				b_pixels_float.resizeErase(height, width);
				framebuffer.insert("B", exr::Slice(exr::PixelType::FLOAT, (char*)(&b_pixels_float[0][0] - dw.min.x - dw.min.y * width), sizeof(b_pixels_float[0][0]) * 1, sizeof(b_pixels_float[0][0]) * width, 1, 1, 0.0, false, false));
			}
		}

		if (a_channel_exists) {
			if (a_channel_format == TextureFormat::MNSY_R16) {
				a_pixels_half.resizeErase(height, width);
				framebuffer.insert("A", exr::Slice(exr::PixelType::HALF, (char*)(&a_pixels_half[0][0] - dw.min.x - dw.min.y * width), sizeof(a_pixels_half[0][0]), sizeof(a_pixels_half[0][0]) * width, 1, 1, 0.0, false, false));
			}
			else if (a_channel_format == TextureFormat::MNSY_R32) {
				a_pixels_float.resizeErase(height, width);
				framebuffer.insert("A", exr::Slice(exr::PixelType::FLOAT, (char*)(&a_pixels_float[0][0] - dw.min.x - dw.min.y * width), sizeof(a_pixels_float[0][0]) * 1, sizeof(a_pixels_float[0][0])* width, 1, 1, 0.0, false, false));
			}
		}



		file.setFrameBuffer(framebuffer);
		file.readPixels(dw.min.y, dw.max.y);


		void* buffer = nullptr;

		// 16 bit half float
		if (channel_format == TextureFormat::MNSY_R16) {

			uint8_t bytesPerSample = sizeof(uint16_t);
			uint16_t bytesPerPixel = (uint16_t)bytesPerSample * (uint16_t)numChannels;
			size_t totalBufferSize = (size_t)bytesPerPixel * (size_t)width * (size_t)height;

			// MEM Alloc
			buffer = malloc(totalBufferSize);

			for (uint8_t c = 1; c <= numChannels; c++) { // loop through r g b a channels

				for (uint16_t h = 0; h < height; h++) {

					for (uint16_t w = 0; w < width; w++) {

						// awsume we want to flip vertically
						uint16_t x = w;
						uint16_t y = height - h - 1;
						if (!flipVertically) {
							y = h;
						}					

						size_t offset = (h * width + w) * numChannels + (c - 1);

						if (c == 1) { // red channel
							
							if(convertToSrgb)
								r_pixels_half[y][x] = (Imath::half)pic_util_linear2srgb_float((float)r_pixels_half[y][x]);

							memcpy((uint16_t*)buffer + offset, &r_pixels_half[y][x], sizeof(uint16_t));
						}
						else if (c == 2) { // green channel


							if (convertToSrgb)
								g_pixels_half[y][x] = (Imath::half)pic_util_linear2srgb_float((float)g_pixels_half[y][x]);

							memcpy((uint16_t*)buffer + offset, &g_pixels_half[y][x], sizeof(uint16_t));
						}
						else if (c == 3) { // blue channel

							if (convertToSrgb)
								b_pixels_half[y][x] = (Imath::half)pic_util_linear2srgb_float((float)b_pixels_half[y][x]);

							memcpy((uint16_t*)buffer + offset, &b_pixels_half[y][x], sizeof(uint16_t));
						}
						else if (c == 4) { // alpha channel

							if (a_channel_format == TextureFormat::MNSY_R16) {
								
								if (convertToSrgb)
									a_pixels_half[y][x] = (Imath::half)pic_util_linear2srgb_float((float)a_pixels_half[y][x]);

								memcpy((uint16_t*)buffer + offset, &a_pixels_half[y][x], sizeof(uint16_t));
							}
							else if (a_channel_format == TextureFormat::MNSY_R32) { 

								// alpha channel will be demoted from float to half
								float valueTemp = a_pixels_float[y][x];

								if (convertToSrgb)
									valueTemp = (Imath::half)pic_util_linear2srgb_float(valueTemp);

								Imath::half value = Imath::half(valueTemp);

								memcpy((uint16_t*)buffer + offset, &value, sizeof(uint16_t));
							}

						}						

					} // loop width
				}	// loop height
			} // loop numChannels

		}
		// 32 bit float
		else if (channel_format == TextureFormat::MNSY_R32) {
			
			uint8_t bytesPerSample = sizeof(float);
			uint16_t bytesPerPixel = (uint16_t)bytesPerSample * (uint16_t)numChannels;
			size_t totalBufferSize = (size_t)bytesPerPixel * width * height;

			buffer = malloc(totalBufferSize);

			for (int c = 1; c <= numChannels; c++) { // loop through r g b a channels

				for (uint16_t h = 0; h < height; h++) {

					for (uint16_t w = 0; w < width; w++) {

						// awsume we want to flip vertically
						uint16_t x = w;
						uint16_t y = height - h - 1;
						if (!flipVertically) {
							y = h;
						}

						size_t offset = (h * width + w) * numChannels + (c - 1);

						if (c == 1) { // red channel

							if(convertToSrgb)
								r_pixels_float[y][x] = pic_util_linear2srgb_float(r_pixels_float[y][x]);

							memcpy((float*)buffer + offset, &r_pixels_float[y][x], sizeof(float));
						}
						else if (c == 2) { // green channel
							
							if (convertToSrgb)
								g_pixels_float[y][x] = pic_util_linear2srgb_float(g_pixels_float[y][x]);

							memcpy((float*)buffer + offset, &g_pixels_float[y][x], sizeof(float));
						}
						else if (c == 3) { // blue channel
							
							if (convertToSrgb)
								b_pixels_float[y][x] = pic_util_linear2srgb_float(b_pixels_float[y][x]);

							memcpy((float*)buffer + offset, &b_pixels_float[y][x], sizeof(float));
						}
						else if (c == 4) { // alpha channel

							if (a_channel_format == TextureFormat::MNSY_R32) {

								if (convertToSrgb)
									a_pixels_float[y][x] = pic_util_linear2srgb_float(a_pixels_float[y][x]);

								memcpy((float*)buffer + offset, &a_pixels_float[y][x], sizeof(float));
							}
							else if (a_channel_format == TextureFormat::MNSY_R16) {

								// alpha channel gets promoted from half to float

								Imath::half valueTemp = a_pixels_half[y][x];

								if (convertToSrgb)
									valueTemp = (Imath::half)pic_util_linear2srgb_float((float)valueTemp);

								float value = (float)valueTemp;

								memcpy((float*)buffer + offset, &value, sizeof(float));
							}
						}
					} // loop width
				} // loop height
			} // loop numChannels
		}

		// fill info struct
		outInfo.width = width;
		outInfo.height = height;
		outInfo.textureFormat = (TextureFormat)((uint8_t)channel_format + (numChannels - 1));
		outInfo.isHalfFloat = channel_format == TextureFormat::MNSY_R16; 
		outInfo.pixels = buffer;

		return outInfo;
	}

	void Picture::WriteExr(PictureError& outPictureError, const char* filepath, const PictureInfo& pictureInfo, const bool flipVertically, const bool convertToLinear) {

		namespace exr = Imf;

		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";


		// first check info struct
		{
			PictureError e = pic_util_check_input_pictureInfo(pictureInfo);
			if (!e.wasSuccessfull) {
				outPictureError.wasSuccessfull = false;
				outPictureError.what = "WriteExr: " + e.what;
				return;
			}
		}
		
		TextureFormat format = pictureInfo.textureFormat;
		uint16_t width = pictureInfo.width;
		uint16_t height = pictureInfo.height;

		uint8_t numChannels = 0, bitsPerChannel = 0, bytesPerPixel = 0;

		TexUtil::get_information_from_textureFormat(format,numChannels,bitsPerChannel,bytesPerPixel);

		TextureFormat channelFormat = (TextureFormat)((uint8_t)format - (numChannels -1));

		bool r_channel_exists = numChannels >= 1;
		bool g_channel_exists = numChannels >= 2;
		bool b_channel_exists = numChannels >= 3;
		bool a_channel_exists = numChannels >= 4;

		exr::Header header(width, height);

		void* r_pixels = nullptr;
		void* g_pixels = nullptr;
		void* b_pixels = nullptr;
		void* a_pixels = nullptr;

		if (channelFormat == TextureFormat::MNSY_R8) {
			// care we will have to promote the values to half_float

			if(r_channel_exists)
				header.channels().insert("R", exr::Channel(exr::PixelType::HALF));
			if(g_channel_exists)
				header.channels().insert("G", exr::Channel(exr::PixelType::HALF));
			if(b_channel_exists)
				header.channels().insert("B", exr::Channel(exr::PixelType::HALF));
			if(a_channel_exists)
				header.channels().insert("A", exr::Channel(exr::PixelType::HALF));
		}
		else if (channelFormat == TextureFormat::MNSY_R16) {
			if (r_channel_exists)
				header.channels().insert("R", exr::Channel(exr::PixelType::HALF));
			if (g_channel_exists)
				header.channels().insert("G", exr::Channel(exr::PixelType::HALF));
			if (b_channel_exists)
				header.channels().insert("B", exr::Channel(exr::PixelType::HALF));
			if (a_channel_exists)
				header.channels().insert("A", exr::Channel(exr::PixelType::HALF));


		}
		else if (channelFormat == TextureFormat::MNSY_R32) {
			if (r_channel_exists)
				header.channels().insert("R", exr::Channel(exr::PixelType::FLOAT));
			if (g_channel_exists)
				header.channels().insert("G", exr::Channel(exr::PixelType::FLOAT));
			if (b_channel_exists)
				header.channels().insert("B", exr::Channel(exr::PixelType::FLOAT));
			if (a_channel_exists)
				header.channels().insert("A", exr::Channel(exr::PixelType::FLOAT));
		}

		exr::OutputFile file(filepath, header);
		exr::FrameBuffer framebuffer;

		exr::PixelType pixelType = exr::PixelType::HALF;
		uint8_t pixelTypeSize = sizeof(uint16_t);

		void* inputBuffer = pictureInfo.pixels;

		if (channelFormat == TextureFormat::MNSY_R8) {

			pixelType = exr::PixelType::HALF;
			pixelTypeSize = sizeof(uint16_t);
			// care we will have to promote the values to half float

			pixelType = exr::PixelType::HALF;
			pixelTypeSize = sizeof(uint16_t);

			Imath::half* r_pixels_half = nullptr;
			Imath::half* g_pixels_half = nullptr;
			Imath::half* b_pixels_half = nullptr;
			Imath::half* a_pixels_half = nullptr;

			size_t channelBufferSize = width * height * sizeof(uint16_t);

			// allocate memory for pixel buffers
			if (r_channel_exists) { r_pixels_half = (Imath::half*)malloc(channelBufferSize); }
			if (g_channel_exists) { g_pixels_half = (Imath::half*)malloc(channelBufferSize); }
			if (b_channel_exists) { b_pixels_half = (Imath::half*)malloc(channelBufferSize); }
			if (a_channel_exists) { a_pixels_half = (Imath::half*)malloc(channelBufferSize); }

			for (uint16_t h = 0; h < height; h++) {
				for (uint16_t w = 0; w < width; w++) {
					for (uint8_t c = 0; c < numChannels; c++) {

						size_t offsetInputBuffer = (h * width + w) * numChannels + c;
						// asume we flip vertically
						uint16_t y = height - h - 1;
						if (!flipVertically) {
							y = h;
						}

						size_t offsetChannelBuffer = y * width + w;

						// promoting the input buffer wich is 8bits to half float // not sure if just casting is how to properly do this
						uint8_t byte = *((uint8_t*)inputBuffer + offsetInputBuffer);
						
						float valueAsFloat = (float)byte;

						Imath::half value = Imath::half(valueAsFloat);

						if (convertToLinear) {
							value = (Imath::half)pic_util_srgb2linear_float((float)value);
						}


						if (c == 0) { // r channel
							memcpy(r_pixels_half + offsetChannelBuffer, &value, sizeof(value));
						}
						else if (c == 1) { // g channel
							memcpy(g_pixels_half + offsetChannelBuffer, &value, sizeof(value));
						}
						else if (c == 2) { // b channel
							memcpy(b_pixels_half + offsetChannelBuffer, &value, sizeof(value));
						}
						else if (c == 3) { // a channel
							memcpy(a_pixels_half + offsetChannelBuffer, &value, sizeof(value));
						}
					} // loop channels
				} // loop width
			} // loop height


			r_pixels = (void*)r_pixels_half;
			g_pixels = (void*)g_pixels_half;
			b_pixels = (void*)b_pixels_half;
			a_pixels = (void*)a_pixels_half;



		}
		else if (channelFormat == TextureFormat::MNSY_R16) {
			
			pixelType = exr::PixelType::HALF;
			pixelTypeSize = sizeof(uint16_t);

			Imath::half* r_pixels_half = nullptr;
			Imath::half* g_pixels_half = nullptr;
			Imath::half* b_pixels_half = nullptr;
			Imath::half* a_pixels_half = nullptr;

			size_t channelBufferSize = width * height * sizeof(uint16_t);

			// allocate memory for pixel buffers
			if (r_channel_exists) { r_pixels_half = (Imath::half*)malloc(channelBufferSize); }
			if (g_channel_exists) {	g_pixels_half = (Imath::half*)malloc(channelBufferSize); }
			if (b_channel_exists) {	b_pixels_half = (Imath::half*)malloc(channelBufferSize); }
			if (a_channel_exists) {	a_pixels_half = (Imath::half*)malloc(channelBufferSize); }

			for (uint16_t h = 0; h < height; h++) {
				for (uint16_t w = 0; w < width; w++) {
					for (uint8_t c = 0; c < numChannels; c++) {

						size_t offsetInputBuffer = (h * width + w) * numChannels + c;
						// asume we flip vertically
						uint16_t y = height - h - 1;
						if (!flipVertically) { 
							y = h; 
						}

						size_t offsetChannelBuffer = y * width + w;

						Imath::half value =  *((Imath::half*)inputBuffer + offsetInputBuffer);
						if (convertToLinear) {
							value = (Imath::half)pic_util_srgb2linear_float((float)value);
						}


						if (c == 0) { // r channel
							memcpy(r_pixels_half + offsetChannelBuffer, &value, sizeof(value));
						}
						else if (c == 1) { // g channel
							memcpy(g_pixels_half + offsetChannelBuffer, &value, sizeof(value));
						}
						else if (c == 2) { // b channel
							memcpy(b_pixels_half + offsetChannelBuffer, &value, sizeof(value));
						}
						else if (c == 3) { // a channel
							memcpy(a_pixels_half + offsetChannelBuffer, &value, sizeof(value));
						}
					} // loop channels
				} // loop width
			} // loop height


			r_pixels = (void*)r_pixels_half;
			g_pixels = (void*)g_pixels_half;
			b_pixels = (void*)b_pixels_half;
			a_pixels = (void*)a_pixels_half;

		}
		else if (channelFormat == TextureFormat::MNSY_R32) {
			
			pixelType = exr::PixelType::FLOAT;
			pixelTypeSize = sizeof(uint32_t);

			float* r_pixels_float = nullptr;
			float* g_pixels_float = nullptr;
			float* b_pixels_float = nullptr;
			float* a_pixels_float = nullptr;

			size_t channelBufferSize = width * height * sizeof(float);

			//allocate memory for pixel buffers
			if (r_channel_exists) {
				r_pixels_float = (float*)malloc(channelBufferSize);
			}
			if (g_channel_exists) {
				g_pixels_float = (float*)malloc(channelBufferSize);
			}
			if (b_channel_exists) {
				b_pixels_float = (float*)malloc(channelBufferSize);
			}
			if (a_channel_exists) {
				a_pixels_float = (float*)malloc(channelBufferSize);
			}

			for (uint16_t h = 0; h < height; h++) {
				for (uint16_t w = 0; w < width; w++) {
					for (uint8_t c = 0; c < numChannels; c++) { 

						size_t offsetInputBuffer = (h * width + w) * numChannels + c;

						// asume we flip vertically
						uint16_t y = height - h -1;

						if (!flipVertically) { y = h; }

						size_t offsetChannelBuffer = y *width + w;

						float value = *((float*)inputBuffer + offsetInputBuffer);
						if (convertToLinear) {
							value = pic_util_srgb2linear_float(value);
						}						

						if (c == 0) { // r channel
							memcpy(r_pixels_float + offsetChannelBuffer, &value, sizeof(value));
						}
						else if (c == 1) { // g channel
							memcpy(g_pixels_float + offsetChannelBuffer, &value, sizeof(value));
						}
						else if (c == 2) { // b channel
							memcpy(b_pixels_float + offsetChannelBuffer, &value, sizeof(value));
						}
						else if (c == 3) { // a channel
							memcpy(a_pixels_float + offsetChannelBuffer, &value, sizeof(value));
						}
					} // loop channels
				} // loop width
			} // loop height


			r_pixels = (void*)r_pixels_float;
			g_pixels = (void*)g_pixels_float;
			b_pixels = (void*)b_pixels_float;
			a_pixels = (void*)a_pixels_float;
		} // TextureFormat::MNSY_R32


		if (r_channel_exists) {
			framebuffer.insert("R", exr::Slice(pixelType, (char*)r_pixels, pixelTypeSize * 1, pixelTypeSize * width));
		}
		if (g_channel_exists) {
			framebuffer.insert("G", exr::Slice(pixelType, (char*)g_pixels, pixelTypeSize * 1, pixelTypeSize * width));
		}
		if (b_channel_exists) {
			framebuffer.insert("B", exr::Slice(pixelType, (char*)b_pixels, pixelTypeSize * 1, pixelTypeSize * width));
		}
		if (a_channel_exists) {
			framebuffer.insert("A", exr::Slice(pixelType, (char*)a_pixels, pixelTypeSize * 1, pixelTypeSize * width));
		}

		file.setFrameBuffer(framebuffer);
		file.writePixels(height);


		if (r_pixels)
			free(r_pixels);
		if (g_pixels)
			free(g_pixels);
		if (b_pixels)
			free(b_pixels);
		if (a_pixels)
			free(a_pixels);

	}

	PictureInfo Picture::ReadHdr(PictureError& outPictureError, const char* filepath, const bool flipVertically, const bool convertToSrgb) {

		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";

		stbi_set_flip_vertically_on_load(flipVertically);
		int width, height, channels;

		float* buffer = stbi_loadf(filepath, &width, &height, &channels, 3);
		
		
		if (convertToSrgb) {

			size_t pixelCount = width * height * channels;

			pic_util_linear2srgb_floatBuffer(buffer, pixelCount);


		} // end convertToSrgb


		return PictureInfo(width,height,graphics::TextureFormat::MNSY_RGB32 ,false,(void*)buffer);
	}

	void Picture::WriteHdr(PictureError& outPictureError, const char* filepath, const PictureInfo& pictureInfo, const bool flipVertically, const bool convertToLinear){

		// initialize outputs

		PictureError err = Picture::pic_util_check_input_pictureInfo(pictureInfo);
		if (!err.wasSuccessfull) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteHdr: " + err.what;
			return;
		}

		int channels = TexUtil::get_channels_amount_from_textureFormat(pictureInfo.textureFormat);

		if (channels == 2) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteHdr: Hdr images do not support dual channel export";
			return;
		}

		// anything but 32 bit will be rejected
		if ((uint8_t)pictureInfo.textureFormat < 9) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteHdr: hdr images only support 32 bit floating point data";
			return;
		}


		float* buffer = (float*)pictureInfo.pixels;

		uint16_t width = pictureInfo.width;
		uint16_t height = pictureInfo.height;

		if (convertToLinear) {
						
			size_t pixelCount = width * height * channels;

			for (int p = 0; p < pixelCount; p++) {

				float val = buffer[p];
				buffer[p] = Picture::pic_util_srgb2linear_float(val);
			}
		}

		stbi_flip_vertically_on_write(flipVertically);
		int stbi_errorCheck = stbi_write_hdr(filepath, pictureInfo.width, pictureInfo.height, channels, buffer);

		if (stbi_errorCheck == 0) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteHdr: stbi_write_hdr() faild to write";
			return;
		}

	}

	PictureInfo Picture::ReadJpg(PictureError& outPictureError, const char* filepath, const bool flipVertically, const bool convertGrayToRGB) {

		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";

		int width, height, channels;

		stbi_set_flip_vertically_on_load(flipVertically);
		void* buffer = (void*)stbi_load(filepath, &width, &height, &channels, 0);
		
		TextureFormat format = TextureFormat::MNSY_R8;

		if (channels == 2) {
			format = TextureFormat::MNSY_RG8;
		}
		if (channels == 3) {
			format = TextureFormat::MNSY_RGB8;
		}

		if (channels == 1 && convertGrayToRGB) {

			format = TextureFormat::MNSY_RGB8;
			void* pixels = malloc(width * height * 3 * sizeof(uint8_t));
						
			for (uint16_t h = 0; h < height; h++) {
				for (uint16_t w = 0; w < width; w++) {

					size_t offsetSrc = (h * width + w);
					size_t offsetDest = offsetSrc * 3; // 3 bc we now have RGB channels in destination buffer

					memcpy((uint8_t*)pixels + offsetDest + 0, (uint8_t*)buffer + offsetSrc, sizeof(uint8_t));
					memcpy((uint8_t*)pixels + offsetDest + 1, (uint8_t*)buffer + offsetSrc, sizeof(uint8_t));
					memcpy((uint8_t*)pixels + offsetDest + 2, (uint8_t*)buffer + offsetSrc, sizeof(uint8_t));					
				}
			}

			free(buffer);
			buffer = pixels;
		}

		PictureInfo outInfo;
		outInfo.width = width;
		outInfo.height = height;
		outInfo.textureFormat = format;
		outInfo.isHalfFloat = false;
		outInfo.pixels = (void*)buffer;

		return outInfo;
	}

	void Picture::WriteJpg(PictureError& outPictureError, const char* filepath, const PictureInfo& pictureInfo, const bool flipVertically) {
		
		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";

		PictureError err =  Picture::pic_util_check_input_pictureInfo(pictureInfo);
		if (!err.wasSuccessfull) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteJpg: " + err.what;
			return;
		}


		int channels = TexUtil::get_channels_amount_from_textureFormat(pictureInfo.textureFormat);

		// alpha channel will be ingored by stbi_write_jpg() so i dont explicitly handle it here.
		if (channels == 2) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteJpg: Jpg images do not support dual channel export";
			return;
		}

		if ((uint8_t)pictureInfo.textureFormat > 8) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteJpg: Jpg images do not support 32 bit floating point data";
			return;
		}


		stbi_flip_vertically_on_write(flipVertically);

		uint16_t width = pictureInfo.width;
		uint16_t height = pictureInfo.height;
		
		void* srcBuffer = pictureInfo.pixels;

		// branch to convert 16 bit to 8 bit data - 
		if ((uint8_t)pictureInfo.textureFormat > 4) {
			
			MNEMOSY_TRACE("Converting 16 bit to 8 bit");

			void* buffer = malloc(width * height * channels * sizeof(uint8_t));
			

			for (uint16_t h = 0; h < height; h++) {
				for (uint16_t w = 0; w < width; w++) {


					// since both src and destination buffers have the exact same layout only different bit depth offset is the same for both
					size_t offset = (h * width + w) * channels;
					
					if (channels >= 1) { // red
						uint16_t value16 = *((uint16_t*)srcBuffer + offset + 0) / (uint16_t)UINT8_MAX;	// scary shit but basically the value at the srcBuffer / 255
						uint8_t value8 = (uint8_t)std::clamp(value16,(uint16_t)0, (uint16_t)UINT8_MAX); // now we clamp it to be sure

						memcpy((uint8_t*)buffer + offset + 0, &value8, sizeof(uint8_t));
					}
					if (channels >= 2) { // green 
						uint16_t value16 = *((uint16_t*)srcBuffer + offset + 1) / (uint16_t)UINT8_MAX;
						uint8_t value8 = (uint8_t)std::clamp(value16, (uint16_t)0, (uint16_t)UINT8_MAX); 

						memcpy((uint8_t*)buffer + offset + 1, &value8, sizeof(uint8_t));
					}
					if (channels >= 3) { // blue
						uint16_t value16 = *((uint16_t*)srcBuffer + offset + 2) / (uint16_t)UINT8_MAX;
						uint8_t value8 = (uint8_t)std::clamp(value16, (uint16_t)0, (uint16_t)UINT8_MAX);

						memcpy((uint8_t*)buffer + offset + 2, &value8, sizeof(uint8_t));
					}
					if (channels == 4) { // alpha
						// since alpha will be discarded anyway we dont care what ends up in alpha channel
						memset((uint8_t*)buffer + offset + 3, 0, sizeof(uint8_t));
					}
				} // loop width
			}// loop heigth


			int stbiErrorCheck = stbi_write_jpg(filepath, pictureInfo.width, pictureInfo.height, channels, buffer, 100);

			// freeing the temporary converted buffer not the src buffer
			free(buffer); 

			if (stbiErrorCheck == 0) {
				outPictureError.wasSuccessfull = false;
				outPictureError.what = "WriteJpg: stbi_write_jpg failed";
			}

			return; // return here is important we dont want to write twice

		} // end if 16 bit

		
		int stbiErrorCheck = stbi_write_jpg(filepath, pictureInfo.width, pictureInfo.height, channels, srcBuffer, 100);

		if (stbiErrorCheck == 0) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteJpg: stbi_write_jpg failed";
			return;
		}

	}

	PictureInfo Picture::ReadPng(PictureError& outPictureError, const char* filepath, const bool flipVertically, const bool convertGrayToRGB) {

		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";

		unsigned int error;		
		unsigned char* pngFile = 0;
		size_t pngsize = 0;

		// grab info first
		unsigned width, height;
		
		// load the entire file contents into pngFile
		error = lodepng_load_file(&pngFile, &pngsize, filepath);
		if (error) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "ReadPng: " + std::string(lodepng_error_text(error));
			return PictureInfo();
		}


		LodePNGState state;
		lodepng_state_init(&state);

		// Inspect the header first
		error = lodepng_inspect(&width, &height, &state, pngFile, pngsize);
		if (error) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "ReadPng: " + std::string(lodepng_error_text(error));
			return PictureInfo();
		}

		// grab info about the file
		LodePNGColorType file_colType = state.info_png.color.colortype;
		uint8_t file_bitDepth = state.info_png.color.bitdepth;

		state.decoder.color_convert = true; // make sure lodepng converts

		TextureFormat format = TextureFormat::MNSY_NONE;

		// state.info_raw  struct is used to tell lodepng in what format we want the data
		if (file_colType == LodePNGColorType::LCT_GREY || file_colType == LodePNGColorType::LCT_GREY_ALPHA) {

			if (!convertGrayToRGB) {

				state.info_raw.colortype = LodePNGColorType::LCT_GREY;

				if (file_bitDepth == 1 || file_bitDepth == 2 || file_bitDepth == 4 || file_bitDepth == 8) {
					// convert to Gray 8bit
					format = TextureFormat::MNSY_R8;
					state.info_raw.bitdepth = 8;
				}
				else if (file_bitDepth == 16) {
					format = TextureFormat::MNSY_R16;
					state.info_raw.bitdepth = 16;
				}
			}
			else {
				state.info_raw.colortype = LodePNGColorType::LCT_RGB;

				if (file_bitDepth == 1 || file_bitDepth == 2 || file_bitDepth == 4 || file_bitDepth == 8) {
					// convert to Gray 8bit
					format = TextureFormat::MNSY_RGB8;
					state.info_raw.bitdepth = 8;
				}
				else if (file_bitDepth == 16) {
					format = TextureFormat::MNSY_RGB16;
					state.info_raw.bitdepth = 16;
				}
			}

		}
		else if (file_colType == LodePNGColorType::LCT_PALETTE) {

			if (file_bitDepth == 1 || file_bitDepth == 2 || file_bitDepth == 4 || file_bitDepth == 8) {
				//  convert to RGB 8bit
				format = TextureFormat::MNSY_RGB8;
				state.info_raw.colortype = LodePNGColorType::LCT_RGB;
				state.info_raw.bitdepth = 8;
			}
		}
		else if (file_colType == LodePNGColorType::LCT_RGB) {

			state.info_raw.colortype = LodePNGColorType::LCT_RGB;

			if (file_bitDepth == 8) {
				format = TextureFormat::MNSY_RGB8;
				state.info_raw.bitdepth = 8;
			}
			else if (file_bitDepth == 16) {
				format = TextureFormat::MNSY_RGB16;
				state.info_raw.bitdepth = 16;
			}
		}
		else if (file_colType == LodePNGColorType::LCT_RGBA) {

			state.info_raw.colortype = LodePNGColorType::LCT_RGBA;

			if (file_bitDepth == 8) {
				format = TextureFormat::MNSY_RGBA8; // this works
				state.info_raw.bitdepth = 8;
			}
			else if (file_bitDepth == 16) {
				format = TextureFormat::MNSY_RGBA16;
				state.info_raw.bitdepth = 16;
			}
		}

		// if for some reason we fail too get a textureFormat we stop and throw an error
		if (format == TextureFormat::MNSY_NONE) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "ReadPng: Texture format could not be determined, format: " + std::to_string(file_colType) +  " , BitDepth: " + std::to_string(file_bitDepth);
			
			if (pngFile) {
				free(pngFile);
			}
			return PictureInfo();
		}

		//now we grab the pixel data that is conveniently converted to the correct format by lodepng
		unsigned char* pixelBuffer;
		error = lodepng_decode(&pixelBuffer, &width, &height, &state, pngFile, pngsize);

		if(error) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "ReadPng: faild to decode image data: \nMessage: " + std::string(lodepng_error_text(error));

			if (pngFile) {
				free(pngFile);
			}
			if (pixelBuffer) {
				free(pixelBuffer);
			}
			return PictureInfo();
		}

		// pixel data is now extracted so we can get rid of the entire file contents
		if (pngFile) {
			free(pngFile);
		}
		lodepng_state_cleanup(&state);		

		uint8_t numChannels, bitsPerChannel, bytesPerPixel;
		TexUtil::get_information_from_textureFormat(format, numChannels, bitsPerChannel, bytesPerPixel);
		
		// Convert from big endian to little endian
		// this is neccesary because png always uses Big endian and lodePng doesn't convert for us
		if (bitsPerChannel == 16) {

			size_t pixelCount = width * height * numChannels;

			pic_util_SwapEndianness(pixelBuffer,0,pixelCount);
		}		

		if (flipVertically) {
			size_t rowSize = width * bytesPerPixel;
			unsigned char* tempRowBuf = static_cast<unsigned char*>(malloc(rowSize));

			uint16_t heightHalf = height / 2;
			uint16_t heightMinusOne = height - 1;

			for (uint32_t h = 0; h < heightHalf; h++) {

				unsigned char* rowTop = pixelBuffer + h * rowSize;
				unsigned char* rowBottom = pixelBuffer + (heightMinusOne - h) * rowSize;

				memcpy(tempRowBuf, rowTop, rowSize); // copy top row to temp
				memcpy(rowTop, rowBottom, rowSize);  // coppy bottom to top
				memcpy(rowBottom, tempRowBuf, rowSize); // copy top from temp to bottom
			}

			free(tempRowBuf);
		}

		// fill info struct 
		PictureInfo info;
		info.isHalfFloat = false;
		info.width = width;
		info.height = height;
		info.textureFormat = format;
		info.pixels = (void*)pixelBuffer;

		return info;
	}

	void Picture::WritePng(PictureError& outPictureError, const char* filepath, const PictureInfo& pictureInfo, const bool flipVertically) {

		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";

		PictureError err = Picture::pic_util_check_input_pictureInfo(pictureInfo);
		if (!err.wasSuccessfull) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WritePng: PictureInfo was not correct or incomplete: " + err.what;
			return;
		}


		TextureFormat format = pictureInfo.textureFormat;
		TextureFormat channelFormat = TexUtil::get_channel_textureFormat(format);

		if (channelFormat == TextureFormat::MNSY_R32) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WritePng: Png files do not support writing 32 bit images.";
			return;
		}

		if (format == TextureFormat::MNSY_RG8 || format == TextureFormat::MNSY_RG16 || format == TextureFormat::MNSY_RG32) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WritePng: Png files do not support writing dual channel images.";
			return;
		}

		unsigned char* pixelBuffer = (unsigned char*)pictureInfo.pixels;

		uint16_t width = pictureInfo.width;
		uint16_t height = pictureInfo.height;

		uint8_t numChannels, bitsPerChannel, bytesPerPixel;
		TexUtil::get_information_from_textureFormat(format, numChannels, bitsPerChannel, bytesPerPixel);

		size_t sizePerRow = (size_t)width * bytesPerPixel;
		size_t bufferSize = sizePerRow * height;
		
		unsigned char* bufferCopy = (unsigned char*)malloc(bufferSize);
		memcpy(bufferCopy, pixelBuffer, bufferSize);

		if (flipVertically) {
			size_t rowSize = width * bytesPerPixel;
			unsigned char* tempRowBuf = static_cast<unsigned char*>(malloc(rowSize));

			uint16_t heightHalf = height / 2;
			uint16_t heightMinusOne = height - 1;

			for (uint32_t h = 0; h < heightHalf; h++) {

				unsigned char* rowTop = bufferCopy + h * rowSize;
				unsigned char* rowBottom = bufferCopy + (heightMinusOne - h) * rowSize;

				memcpy(tempRowBuf, rowTop, rowSize); // copy top row to temp
				memcpy(rowTop, rowBottom, rowSize);  // coppy bottom to top
				memcpy(rowBottom, tempRowBuf, rowSize); // copy top from temp to bottom
			}

			free(tempRowBuf);
		}

		//convert little to big endian for 16 bit images
		if (bitsPerChannel == 16) {

			size_t pixelCount = width * height * numChannels;

			pic_util_SwapEndianness(bufferCopy, 0, pixelCount);
		}


		LodePNGState state;
		lodepng_state_init(&state);

		// When using the LodePNGState, it uses the following fields for encoding:
		// LodePNGInfo info_png: here you specify how you want the PNG (the output) to be.
		// LodePNGColorMode info_raw: here you say what color type of the raw image (the input) has
		// LodePNGEncoderSettings encoder: you can specify a few settings for the encoder to use
		
		state.encoder.auto_convert = 0; // if 0 uses info_png
		state.encoder.zlibsettings.btype = 0; // disable compression

		switch (format)
		{
		case mnemosy::graphics::MNSY_R8:

			state.info_raw.colortype = LodePNGColorType::LCT_GREY;
			state.info_raw.bitdepth = 8;
			break;
		case mnemosy::graphics::MNSY_RGB8:
			
			state.info_raw.colortype = LodePNGColorType::LCT_RGB;
			state.info_raw.bitdepth = 8;
			break;
		case mnemosy::graphics::MNSY_RGBA8:

			state.info_raw.colortype = LodePNGColorType::LCT_RGBA;
			state.info_raw.bitdepth = 8;
			break;
		case mnemosy::graphics::MNSY_R16:
			state.info_raw.colortype = LodePNGColorType::LCT_GREY;
			state.info_raw.bitdepth = 16;
			break;
		case mnemosy::graphics::MNSY_RGB16:
			state.info_raw.colortype = LodePNGColorType::LCT_RGB;
			state.info_raw.bitdepth = 16;
			break;
		case mnemosy::graphics::MNSY_RGBA16:
			state.info_raw.colortype = LodePNGColorType::LCT_RGBA;
			state.info_raw.bitdepth = 16;
			break;
		}

		// output should equal input
		state.info_png.color.colortype = state.info_raw.colortype;
		state.info_png.color.bitdepth = state.info_raw.bitdepth;
				

		unsigned int error = 0;
		unsigned char* pngFile = 0;
		size_t pngFileSize = 0;

		error = lodepng_encode(&pngFile,&pngFileSize,bufferCopy,width,height,&state);
		if (error) {

			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WritePng: Failed to encode image data. Message: " + std::string(lodepng_error_text(error));
			if (pngFile) {
				free(pngFile);
			}
			return;
		}

		if (bufferCopy) {
			free(bufferCopy);
		}


		error = lodepng_save_file(pngFile, pngFileSize, filepath);
		if (error) {

			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WritePng: Failed to save file. Message: " + std::string(lodepng_error_text(error));
			if (pngFile) {
				free(pngFile);
			}
			return;
		}

		lodepng_state_cleanup(&state);

		if (pngFile) {
			free(pngFile);
		}
	}

	void Picture::WriteKtx2(PictureError& outPictureError, const char* filepath, const PictureInfo& pictureInfo, const bool flipVertically)
	{

		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";

		PictureError err = Picture::pic_util_check_input_pictureInfo(pictureInfo);
		if (!err.wasSuccessfull) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteKtx2: PictureInfo was not correct or incomplete: " + err.what;
			return;
		}

		graphics::KtxImage ktxImage;

		unsigned int errorCode =  ktxImage.Save_WithoutMips(filepath,pictureInfo.pixels,flipVertically,pictureInfo.textureFormat,pictureInfo.width,pictureInfo.height,pictureInfo.isHalfFloat);

		if (errorCode != 0) {

			outPictureError.wasSuccessfull = false;
			outPictureError.what = "WriteKtx2: Failed " + std::string(ktxErrorString((ktx_error_code_e)errorCode));
			return;
		}

	}

	// =========== Util Methods ==============

	PictureInfo Picture::STB_Read(PictureError& outPictureError, const char* filepath, const bool flipVertically) {
		// initialize outputs
		outPictureError.wasSuccessfull = true;
		outPictureError.what = "";

		std::filesystem::path p = { filepath };
		
		if (!std::filesystem::exists(p)) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "STB_Read: Filepath does not exists";

			return PictureInfo();
		}



		int width = 0, height = 0, channels = 0;
		
		stbi_set_flip_vertically_on_load(flipVertically);
		unsigned char* buffer = stbi_load(filepath, &width, &height, &channels, 4);


		if (buffer == nullptr) {
			outPictureError.wasSuccessfull = false;
			outPictureError.what = "STB_Read: Unable to read image";

			return PictureInfo();
		}

		PictureInfo outInfo;
		outInfo.width = width;
		outInfo.height = height;
		outInfo.textureFormat = graphics::TextureFormat::MNSY_RGB8;
		if (channels == 4) {
			outInfo.textureFormat = graphics::TextureFormat::MNSY_RGBA8;
		}
		outInfo.isHalfFloat = false;
		outInfo.pixels = (void*)buffer;

		return outInfo;
	}

	PictureError Picture::pic_util_check_input_pictureInfo(const PictureInfo& pictureInfo) {
		
		if (pictureInfo.pixels == nullptr) {
			return PictureError(false, "pictureInfo does not contain any pixel data");
		}

		if (pictureInfo.textureFormat == TextureFormat::MNSY_NONE) {
			return PictureError(false, "pictureInfo format is not specified.");
		}

		if (pictureInfo.height == 0 || pictureInfo.width == 0) {
			return PictureError(false, "pictureInfo has either width or height set to 0.");
		}

		return PictureError(true, "");
	}

	float Picture::pic_util_linear2srgb_float(float linearValue) {

		if(linearValue <= 0.0031308f){
	    	return linearValue * 12.92f;
	  	}
	    
	  	return 1.055f* std::pow(linearValue,(1.0f / 2.4f) ) - 0.055f;
	}

	float Picture::pic_util_srgb2linear_float(float srgbValue) {

		if(srgbValue <= 0.04045f){
		    return srgbValue/12.92f;
		 }

		 return std::pow( (srgbValue + 0.055f)/ 1.055f, 2.4f);
	}

}