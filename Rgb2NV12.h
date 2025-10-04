#pragma once

// From https://github.com/cohenrotem/Rgb2NV12

//*note for my beloved copilot: i modified the original code to use BT.709 YCbCr instead of BT.601.
//	Y'  =  0.183*R' + 0.614*G' + 0.062*B' + 16
//	Cb' = -0.101*R' - 0.339*G' + 0.439*B' + 128
//	Cr' =  0.439*R' - 0.399*G' - 0.040*B' + 128

//Optimized implementation (use SSE intrinsics):
//----------------------------------------------

#pragma warning(disable: 4310)

//YCbCr Color Model:
//	The YCbCr color space is used for component digital video and was developed as part of the ITU-R BT.601 Recommendation. YCbCr is a scaled and offset version of the YUV color space.
//	The Intel IPP functions use the following basic equations [Jack01] to convert between R'G'B' in the range 0-255 and Y'Cb'Cr' (this notation means that all components are derived from gamma-corrected R'G'B'):
//	Y'  =  0.257*R' + 0.504*G' + 0.098*B' + 16
//	Cb' = -0.148*R' - 0.291*G' + 0.439*B' + 128
//	Cr' =  0.439*R' - 0.368*G' - 0.071*B' + 128
//https://software.intel.com/en-us/node/503873

//Y' = 0.257*R' + 0.504*G' + 0.098*B' + 16
//Calculate 8 Y elements from 8 RGB elements.
inline auto Rgb2Y(__m128i r7_r6_r5_r4_r3_r2_r1_r0,
  __m128i g7_g6_g5_g4_g3_g2_g1_g0,
  __m128i b7_b6_b5_b4_b3_b2_b1_b0)
{
  //Each coefficient is expanded by 2^15, and rounded to int16 (add 0.5 for rounding).
  auto const r_coef{ _mm_set1_epi16((short)(0.183 * 32768.0 + 0.5)) };  //8 coefficients - R scale factor.
  auto const g_coef{ _mm_set1_epi16((short)(0.614 * 32768.0 + 0.5)) };  //8 coefficients - G scale factor.
  auto const b_coef{ _mm_set1_epi16((short)(0.062 * 32768.0 + 0.5)) };  //8 coefficients - B scale factor.
  const auto ofs{ _mm_set1_epi16(16 * 64 + 32) }; //8 Offset coefficients. Multiply by 64 because inputs get multiplied (add 32 for later rounding).

  //Multiply input elements by 64 for improved accuracy.
  r7_r6_r5_r4_r3_r2_r1_r0 = _mm_slli_epi16(r7_r6_r5_r4_r3_r2_r1_r0, 6);
  g7_g6_g5_g4_g3_g2_g1_g0 = _mm_slli_epi16(g7_g6_g5_g4_g3_g2_g1_g0, 6);
  b7_b6_b5_b4_b3_b2_b1_b0 = _mm_slli_epi16(b7_b6_b5_b4_b3_b2_b1_b0, 6);

  //Use the special intrinsic _mm_mulhrs_epi16 that calculates round(r*r_coef/2^15).
  //Calculate Y' = 0.257*R' + 0.504*G' + 0.098*B' + (16*64 + 32) (use fixed point computations)
  auto y7_y6_y5_y4_y3_y2_y1_y0{ _mm_add_epi16(_mm_add_epi16(_mm_add_epi16(
    _mm_mulhrs_epi16(r7_r6_r5_r4_r3_r2_r1_r0, r_coef),
    _mm_mulhrs_epi16(g7_g6_g5_g4_g3_g2_g1_g0, g_coef)),
    _mm_mulhrs_epi16(b7_b6_b5_b4_b3_b2_b1_b0, b_coef)),
    ofs) };

  //Divide result by 64.
  y7_y6_y5_y4_y3_y2_y1_y0 = _mm_srli_epi16(y7_y6_y5_y4_y3_y2_y1_y0, 6);

  return y7_y6_y5_y4_y3_y2_y1_y0;
}



//Pack two signed int16 elements to 32 bits element. a is the high 16 bits.
#define PACK_INT16_PAIR(a,b)    ((((unsigned int)(a)) << 16u) | ((unsigned int)(b) & 0xFFFF))

//U equals Cb' = -0.148*R' - 0.291*G' + 0.439*B' + 128
//V equals Cr' =  0.439*R' - 0.368*G' - 0.071*B' + 128
//Convert duplicated 4 pairs of R,G,B elements to 4 interleaved U,V elements.
//Add offset 128*4 instead of 128 (because input elements are actually sum of 4 color elements).
//Divide the result by 4 (with rounding).
inline __m128i Rgb2UV(__m128i r3_r3_r2_r2_r1_r1_r0_r0,
  __m128i g3_g3_g2_g2_g1_g1_g0_g0,
  __m128i b3_b3_b2_b2_b1_b1_b0_b0)
{
  //Each coefficient is expanded by 2^15, and rounded to int16 (add or subtract 0.5 for rounding).
  //Coefficient are interleaved: low part is U scale factor, and high part is V scale factor.
  //Remark: I used _mm_set1_epi32 with ugly packing macro instead of _mm_set_epi16(...), for helping the compiler...
  //For example r_coef = (0.439, -0.148, 0.439, -0.148, 0.439, -0.148, 0.439, -0.148) expanded by 2^15 and by 2.
  //Expand coefficient by 2 for improved accuracy. 
  auto const r_coef{ _mm_set1_epi32((int)PACK_INT16_PAIR((short)(0.439 * 2.0 * 32768.0 + 0.5), (short)(-0.101 * 2.0 * 32768.0 - 0.5))) };  //8 coefficients - R scale factor.
  auto const g_coef{ _mm_set1_epi32((int)PACK_INT16_PAIR((short)(-0.399 * 2.0 * 32768.0 - 0.5), (short)(-0.339 * 2.0 * 32768.0 - 0.5))) };  //8 coefficients - G scale factor.
  auto const b_coef{ _mm_set1_epi32((int)PACK_INT16_PAIR((short)(-0.040 * 2.0 * 32768.0 - 0.5), (short)(0.439 * 2.0 * 32768.0 + 0.5))) };  //8 coefficients - B scale factor.
  auto const ofs{ _mm_set1_epi16(128 * 8 + 4) }; //8 Offset coefficients. Add 4 for rounding before dividing by 8.

  //Use the special intrinsic _mm_mulhrs_epi16 that calculates round(r*r_coef/2^15).
  //Calculate 4*Cb' = -0.148*R' - 0.291*G' + 0.439*B' + 128*4 (use fixed point computations)
  //Calculate 4*Cr' =  0.439*R' - 0.368*G' - 0.071*B' + 128*4 (use fixed point computations)
  auto v3_u3_v2_u2_v1_u1_v0_u0{ _mm_add_epi16(_mm_add_epi16(_mm_add_epi16(
    _mm_mulhrs_epi16(r3_r3_r2_r2_r1_r1_r0_r0, r_coef),
    _mm_mulhrs_epi16(g3_g3_g2_g2_g1_g1_g0_g0, g_coef)),
    _mm_mulhrs_epi16(b3_b3_b2_b2_b1_b1_b0_b0, b_coef)),
    ofs) };

  //Divide result by 8 (by 4 because source is sum of 4 elements and by 2 because coefficient are expanded by 2).
  v3_u3_v2_u2_v1_u1_v0_u0 = _mm_srli_epi16(v3_u3_v2_u2_v1_u1_v0_u0, 3);

  return v3_u3_v2_u2_v1_u1_v0_u0;
}

//Convert from RGBRGBRGB... to RRR..., GGG..., BBB...
//Input: Two SSE registers (24 uint8 elements) ordered RGBRGB...
//Output: Three SSE registers ordered RRR..., GGG... and BBB...
//        Unpack the result from uint8 elements to uint16 elements.
inline auto GatherRGBx8(__m128i const &r5_b4_g4_r4_b3_g3_r3_b2_g2_r2_b1_g1_r1_b0_g0_r0,
  __m128i const &b7_g7_r7_b6_g6_r6_b5_g5,
  __m128i &r7_r6_r5_r4_r3_r2_r1_r0,
  __m128i &g7_g6_g5_g4_g3_g2_g1_g0,
  __m128i &b7_b6_b5_b4_b3_b2_b1_b0)
{
  //Shuffle mask for gathering 4 R elements, 4 G elements and 4 B elements (also set last 4 elements to duplication of first 4 elements).
  auto const shuffle_mask{ _mm_set_epi8(9, 6, 3, 0, 11, 8, 5, 2, 10, 7, 4, 1, 9, 6, 3, 0) };

  auto b7_g7_r7_b6_g6_r6_b5_g5_r5_b4_g4_r4{ _mm_alignr_epi8(b7_g7_r7_b6_g6_r6_b5_g5, r5_b4_g4_r4_b3_g3_r3_b2_g2_r2_b1_g1_r1_b0_g0_r0, 12) };

  //Gather 4 R elements, 4 G elements and 4 B elements.
  //Remark: As I recall _mm_shuffle_epi8 instruction is not so efficient (I think execution is about 5 times longer than other shuffle instructions).
  auto r3_r2_r1_r0_b3_b2_b1_b0_g3_g2_g1_g0_r3_r2_r1_r0{ _mm_shuffle_epi8(r5_b4_g4_r4_b3_g3_r3_b2_g2_r2_b1_g1_r1_b0_g0_r0, shuffle_mask) };
  auto r7_r6_r5_r4_b7_b6_b5_b4_g7_g6_g5_g4_r7_r6_r5_r4{ _mm_shuffle_epi8(b7_g7_r7_b6_g6_r6_b5_g5_r5_b4_g4_r4, shuffle_mask) };

  //Put 8 R elements in lower part.
  auto b7_b6_b5_b4_g7_g6_g5_g4_r7_r6_r5_r4_r3_r2_r1_r0{ _mm_alignr_epi8(r7_r6_r5_r4_b7_b6_b5_b4_g7_g6_g5_g4_r7_r6_r5_r4, r3_r2_r1_r0_b3_b2_b1_b0_g3_g2_g1_g0_r3_r2_r1_r0, 12) };

  //Put 8 G elements in lower part.
  auto g3_g2_g1_g0_r3_r2_r1_r0_zz_zz_zz_zz_zz_zz_zz_zz{ _mm_slli_si128(r3_r2_r1_r0_b3_b2_b1_b0_g3_g2_g1_g0_r3_r2_r1_r0, 8) };
  auto zz_zz_zz_zz_r7_r6_r5_r4_b7_b6_b5_b4_g7_g6_g5_g4{ _mm_srli_si128(r7_r6_r5_r4_b7_b6_b5_b4_g7_g6_g5_g4_r7_r6_r5_r4, 4) };
  auto r7_r6_r5_r4_b7_b6_b5_b4_g7_g6_g5_g4_g3_g2_g1_g0{ _mm_alignr_epi8(zz_zz_zz_zz_r7_r6_r5_r4_b7_b6_b5_b4_g7_g6_g5_g4, g3_g2_g1_g0_r3_r2_r1_r0_zz_zz_zz_zz_zz_zz_zz_zz, 12) };

  //Put 8 B elements in lower part.
  auto b3_b2_b1_b0_g3_g2_g1_g0_r3_r2_r1_r0_zz_zz_zz_zz{ _mm_slli_si128(r3_r2_r1_r0_b3_b2_b1_b0_g3_g2_g1_g0_r3_r2_r1_r0, 4) };
  auto zz_zz_zz_zz_zz_zz_zz_zz_r7_r6_r5_r4_b7_b6_b5_b4{ _mm_srli_si128(r7_r6_r5_r4_b7_b6_b5_b4_g7_g6_g5_g4_r7_r6_r5_r4, 8) };
  auto zz_zz_zz_zz_r7_r6_r5_r4_b7_b6_b5_b4_b3_b2_b1_b0{ _mm_alignr_epi8(zz_zz_zz_zz_zz_zz_zz_zz_r7_r6_r5_r4_b7_b6_b5_b4, b3_b2_b1_b0_g3_g2_g1_g0_r3_r2_r1_r0_zz_zz_zz_zz, 12) };

  //Unpack uint8 elements to uint16 elements.
  r7_r6_r5_r4_r3_r2_r1_r0 = _mm_cvtepu8_epi16(b7_b6_b5_b4_g7_g6_g5_g4_r7_r6_r5_r4_r3_r2_r1_r0);
  g7_g6_g5_g4_g3_g2_g1_g0 = _mm_cvtepu8_epi16(r7_r6_r5_r4_b7_b6_b5_b4_g7_g6_g5_g4_g3_g2_g1_g0);
  b7_b6_b5_b4_b3_b2_b1_b0 = _mm_cvtepu8_epi16(zz_zz_zz_zz_r7_r6_r5_r4_b7_b6_b5_b4_b3_b2_b1_b0);
}


//Convert two rows from RGB to two Y rows, and one row of interleaved U,V.
//I0 and I1 points two sequential source rows.
//I0 -> rgbrgbrgbrgbrgbrgb...
//I1 -> rgbrgbrgbrgbrgbrgb...
//Y0 and Y1 points two sequential destination rows of Y plane.
//Y0 -> yyyyyy
//Y1 -> yyyyyy
//UV0 points destination rows of interleaved UV plane.
//UV0 -> uvuvuv
inline auto Rgb2NV12TwoRows(unsigned char const I0[],
  unsigned char const I1[],
  int const &image_width,
  unsigned char Y0[],
  unsigned char Y1[],
  unsigned char UV0[])
{
  int srcx;   //Index in I0 and I1.
  __m128i i0__r7_r6_r5_r4_r3_r2_r1_r0, i1__r7_r6_r5_r4_r3_r2_r1_r0;
  __m128i i0__g7_g6_g5_g4_g3_g2_g1_g0, i1__g7_g6_g5_g4_g3_g2_g1_g0;
  __m128i i0__b7_b6_b5_b4_b3_b2_b1_b0, i1__b7_b6_b5_b4_b3_b2_b1_b0;

  srcx = 0;

  //Process 16 source pixels per iteration (8 pixels of row I0 and 8 pixels of row I1).
  for (auto x{ 0 } /* Column index */; x < image_width; x += 8)
  {
    //Load 8 elements of each color channel R,G,B from first row.
    auto i0__r5_b4_g4_r4_b3_g3_r3_b2_g2_r2_b1_g1_r1_b0_g0_r0{ _mm_loadu_si128((__m128i *) & I0[srcx]) };     //Unaligned load of 16 uint8 elements
    auto i0__b7_g7_r7_b6_g6_r6_b5_g5{ _mm_loadu_si128((__m128i *) & I0[srcx + 16]) };  //Unaligned load of (only) 8 uint8 elements (lower half of SSE register).

    //Separate RGB, and put together R elements, G elements and B elements (together in same SSE register).
    //Result is also unpacked from uint8 to uint16 elements.
    GatherRGBx8(i0__r5_b4_g4_r4_b3_g3_r3_b2_g2_r2_b1_g1_r1_b0_g0_r0,
      i0__b7_g7_r7_b6_g6_r6_b5_g5,
      i0__r7_r6_r5_r4_r3_r2_r1_r0,
      i0__g7_g6_g5_g4_g3_g2_g1_g0,
      i0__b7_b6_b5_b4_b3_b2_b1_b0);

    //Load R,G,B elements from second row.
    auto i1__r5_b4_g4_r4_b3_g3_r3_b2_g2_r2_b1_g1_r1_b0_g0_r0{ _mm_loadu_si128((__m128i *) & I1[srcx]) };     //Unaligned load of 16 uint8 elements
    auto i1__b7_g7_r7_b6_g6_r6_b5_g5{ _mm_loadu_si128((__m128i *) & I1[srcx + 16]) };  //Unaligned load of (only) 8 uint8 elements (lower half of SSE register).

    //Separate
    GatherRGBx8(i1__r5_b4_g4_r4_b3_g3_r3_b2_g2_r2_b1_g1_r1_b0_g0_r0,
      i1__b7_g7_r7_b6_g6_r6_b5_g5,
      i1__r7_r6_r5_r4_r3_r2_r1_r0,
      i1__g7_g6_g5_g4_g3_g2_g1_g0,
      i1__b7_b6_b5_b4_b3_b2_b1_b0);

    //Calculate 8 Y elements.
    auto i0__y7_y6_y5_y4_y3_y2_y1_y0{ Rgb2Y(i0__r7_r6_r5_r4_r3_r2_r1_r0,
      i0__g7_g6_g5_g4_g3_g2_g1_g0,
      i0__b7_b6_b5_b4_b3_b2_b1_b0) };

    //Calculate 8 Y elements (second row).
    auto i1__y7_y6_y5_y4_y3_y2_y1_y0{ Rgb2Y(i1__r7_r6_r5_r4_r3_r2_r1_r0,
      i1__g7_g6_g5_g4_g3_g2_g1_g0,
      i1__b7_b6_b5_b4_b3_b2_b1_b0) };

    //Pack uint16 elements to 16 uint8 elements (put result in single SSE register).
    auto y7_y6_y5_y4_y3_y2_y1_y0___y7_y6_y5_y4_y3_y2_y1_y0{ _mm_packus_epi16(i0__y7_y6_y5_y4_y3_y2_y1_y0, i1__y7_y6_y5_y4_y3_y2_y1_y0) };

    //Store 8 elements of Y in row Y0, and 8 elements of Y in row Y1.
    _mm_storel_epi64((__m128i *) & Y0[x], y7_y6_y5_y4_y3_y2_y1_y0___y7_y6_y5_y4_y3_y2_y1_y0);
    _mm_storel_epi64((__m128i *) & Y1[x], _mm_srli_si128(y7_y6_y5_y4_y3_y2_y1_y0___y7_y6_y5_y4_y3_y2_y1_y0, 8));


    //Calculate U and V elements
    //Instead of computing 4 elements of U,V and then average of 2x2:
    //1. Compute sum of 2x2 of R, G and B
    //2. Convert to sum to U,V.
    //3. Divide the sum by 4 (convert sum to average of 4 elements).
    //////////////////////////////////////////////////////////////////////////

    //Vertical sum (sum elements from two rows).
    auto sum_i0i1__r7_r6_r5_r4_r3_r2_r1_r0{ _mm_add_epi16(i0__r7_r6_r5_r4_r3_r2_r1_r0, i1__r7_r6_r5_r4_r3_r2_r1_r0) };
    auto sum_i0i1__g7_g6_g5_g4_g3_g2_g1_g0{ _mm_add_epi16(i0__g7_g6_g5_g4_g3_g2_g1_g0, i1__g7_g6_g5_g4_g3_g2_g1_g0) };
    auto sum_i0i1__b7_b6_b5_b4_b3_b2_b1_b0{ _mm_add_epi16(i0__b7_b6_b5_b4_b3_b2_b1_b0, i1__b7_b6_b5_b4_b3_b2_b1_b0) };

    //Horizontal sum (sum two sequential elements - sum elements from same row)
    //Horizontal sum sum_i0i1__r7_r6_r5_r4_r3_r2_r1_r0 with itself (waste upper 4 elements).
    //Each uint16 element is sum of 2x2 color elements.
    auto sum_i0i1__r67_r45_r23_r01_r67_r45_r23_r01{ _mm_hadd_epi16(sum_i0i1__r7_r6_r5_r4_r3_r2_r1_r0, sum_i0i1__r7_r6_r5_r4_r3_r2_r1_r0) };
    auto sum_i0i1__g67_g45_g23_g01_g67_g45_g23_g01{ _mm_hadd_epi16(sum_i0i1__g7_g6_g5_g4_g3_g2_g1_g0, sum_i0i1__g7_g6_g5_g4_g3_g2_g1_g0) };
    auto sum_i0i1__b67_b45_b23_b01_b67_b45_b23_b01{ _mm_hadd_epi16(sum_i0i1__b7_b6_b5_b4_b3_b2_b1_b0, sum_i0i1__b7_b6_b5_b4_b3_b2_b1_b0) };

    //Interleave low parts of same register - duplicate each uint16 element twice.
    //This is a preparation for interleaved U and V elements.
    auto sum_i0i1__r67_r67_r45_r45_r23_r23_r01_r01{ _mm_unpacklo_epi16(sum_i0i1__r67_r45_r23_r01_r67_r45_r23_r01, sum_i0i1__r67_r45_r23_r01_r67_r45_r23_r01) };
    auto sum_i0i1__g67_g67_g45_g45_g23_g23_g01_g01{ _mm_unpacklo_epi16(sum_i0i1__g67_g45_g23_g01_g67_g45_g23_g01, sum_i0i1__g67_g45_g23_g01_g67_g45_g23_g01) };
    auto sum_i0i1__b67_b67_b45_b45_b23_b23_b01_b01{ _mm_unpacklo_epi16(sum_i0i1__b67_b45_b23_b01_b67_b45_b23_b01, sum_i0i1__b67_b45_b23_b01_b67_b45_b23_b01) };

    //Calculate 8 interleaved U,V elements.
    //Rgb2UV convert RGB to U,V and divide result by 4 (with rounding).
    auto v3_u3_v2_u2_v1_u1_v0_u0{ Rgb2UV(sum_i0i1__r67_r67_r45_r45_r23_r23_r01_r01,
      sum_i0i1__g67_g67_g45_g45_g23_g23_g01_g01,
      sum_i0i1__b67_b67_b45_b45_b23_b23_b01_b01) };

    //Convert uint16 elements to uint8 elements (pack with itself, and ignore upper 8 uint8 elements of the result).
    v3_u3_v2_u2_v1_u1_v0_u0 = _mm_packus_epi16(v3_u3_v2_u2_v1_u1_v0_u0, v3_u3_v2_u2_v1_u1_v0_u0);

    //Store 8 elements of interleaved  UV in row UV0.
    _mm_storel_epi64((__m128i *) & UV0[x], v3_u3_v2_u2_v1_u1_v0_u0);

    srcx += 24; //Advance 24 source bytes per iteration.
  }
}


//Convert image I from pixel ordered RGB to NV12 format.
//Implementation uses SSE intrinsics for performance optimization.
//Use fixed point computations for better performance.
//I - Input image in pixel ordered RGB format.
//image_width - Number of columns of I.
//image_height - Number of rows of I.
//J - Destination "image" in NV12 format.

//I is pixel ordered RGB color format (size in bytes is image_width*image_height*3):
//RGBRGBRGBRGBRGBRGB
//RGBRGBRGBRGBRGBRGB
//RGBRGBRGBRGBRGBRGB
//RGBRGBRGBRGBRGBRGB
//
//J is in NV12 format (size in bytes is image_width*image_height*3/2):
//YYYYYY
//YYYYYY
//UVUVUV
//Each element of destination U is average of 2x2 "original" U elements.
//Each element of destination V is average of 2x2 "original" V elements.
//
//Limitations:
//1. image_width must be a multiple of 8.
//2. image_height must be a multiple of 2.
//3. I and J must be two separate arrays (in place computation is not supported).
//
//Comments:
//1. Code uses SSE 4.1 instruction set.
//   Better performance can be archived using AVX2 implementation.
//   (AVX2 is supported by Intel Core 4'th generation and above, and new AMD processors).
//2. The code is not the best SSE optimization:
//   Uses unaligned load and store operations.
//   Utilize only half SSE register in few cases.
//   Instruction selection is probably sub-optimal.
//3. Performance:
//   In my Intel Core i5 2'en Gen (Ivy Bridge) performance is about the same as IPP (when using Intel Compiler 15.0).
//   Using Microsoft Visual Studio 2013 Compiler, execution time is about 10% higher than IPP.
//   Purpose is not to compete IPP for performance...
//4. Purpose: Code can be used as a base for things not supported by IPP (converting BGR to NV12 for example).
//5. Don't get scared by the long variable names (like sum_i0i1__r67_r67_r45_r45_r23_r23_r01_r01).
//   The long names makes it easier to follow what SSE instructions do.
inline auto Rgb2NV12_useSSE(unsigned char const I[],
  int const &image_width,
  int const &image_height,
  unsigned char J[])
{
  //In NV12 format, UV plane starts below Y plane.
  auto *UV = &J[image_width * image_height];

  //I0 and I1 points two sequential source rows.
  unsigned char const *I0;  //I0 -> rgbrgbrgbrgbrgbrgb...
  unsigned char const *I1;  //I1 -> rgbrgbrgbrgbrgbrgb...

  //Y0 and Y1 points two sequential destination rows of Y plane.
  unsigned char *Y0;	//Y0 -> yyyyyy
  unsigned char *Y1;	//Y1 -> yyyyyy

  //UV0 points destination rows of interleaved UV plane.
  unsigned char *UV0; //UV0 -> uvuvuv

  //In each iteration: process two rows of Y plane, and one row of interleaved UV plane.
  for (auto y{ 0 } /* Row index */; y < image_height; y += 2)
  {
    I0 = &I[y * image_width * 3];		//Input row width is image_width*3 bytes (each pixel is R,G,B).
    I1 = &I[(y + 1) * image_width * 3];

    Y0 = &J[y * image_width];			//Output Y row width is image_width bytes (one Y element per pixel).
    Y1 = &J[(y + 1) * image_width];

    UV0 = &UV[(y / 2) * image_width];	//Output UV row - width is same as Y row width.

    //Process two source rows into: Two Y destination row, and one destination interleaved U,V row.
    Rgb2NV12TwoRows(I0,
      I1,
      image_width,
      Y0,
      Y1,
      UV0);
  }
}

inline void Bgr2RgbAndReverseUpDown(unsigned char const I[],
  int const &image_width,
  int const &image_height,
  unsigned char J[])
{
  unsigned char const *I0;
  unsigned char *J0;

  for (auto y{ 0 }; y < image_height; ++y)
  {
    I0 = &I[y * image_width * 3];
    J0 = &J[(image_height - 1 - y) * image_width * 3];
    //memcpy(J0, I0, image_width*3);

    for (auto x{ 0 }; x < image_width; ++x)
    {
      J0[x * 3 + 2] = I0[x * 3 + 0];	//B
      J0[x * 3 + 1] = I0[x * 3 + 1];	//G
      J0[x * 3 + 0] = I0[x * 3 + 2];	//R
    }
  }
}