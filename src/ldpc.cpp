/*
 * ldpc.c
 *
 *  Created on: Sep 25, 2019
 *      Author: user
 */

//
// LDPC decoder for FT8.
//
// given a 174-bit codeword as an array of log-likelihood of zero,
// return a 174-bit corrected codeword, or zero-length array.
// last 87 bits are the (systematic) plain-text.
// this is an implementation of the sum-product algorithm
// from Sarah Johnson's Iterative Error Correction book.
// codeword[i] = log ( P(x=0) / P(x=1) )
//
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "constants.h"

static int ldpc_check(uint8_t codeword[]);
static float fast_tanh(float x);
static float fast_atanh(float x);

// REINSERTED FOR TEST
static float ldpc_m[83][174];
static float ldpc_e[83][174];

// Packs a string of bits each represented as a zero/non-zero byte in plain[],
// as a string of packed bits starting from the MSB of the first byte of packed[]
void pack_bits(const uint8_t plain[], int num_bits, uint8_t packed[])
{
  int num_bytes = (num_bits + 7) / 8;
  for (int i = 0; i < num_bytes; ++i)
  {
    packed[i] = 0;
  }

  uint8_t mask = 0x80;
  int byte_idx = 0;
  for (int i = 0; i < num_bits; ++i)
  {
    if (plain[i])
    {
      packed[byte_idx] |= mask;
    }
    mask >>= 1;
    if (!mask)
    {
      mask = 0x80;
      ++byte_idx;
    }
  }
}

//
// does a 174-bit codeword pass the FT8's LDPC parity checks?
// returns the number of parity errors.
// 0 means total success.
//
static int ldpc_check(uint8_t codeword[])
{
  int errors = 0;

  for (int j = 0; j < M; ++j)
  {
    uint8_t x = 0;
    for (int i = 0; i < kNrw[j]; ++i)
    {
      x ^= codeword[kNm[j][i] - 1];
    }
    if (x != 0)
    {
      ++errors;
    }
  }
  return errors;
}

void bp_decode(float codeword[], int max_iters, uint8_t plain[], int *ok)
{
  ldpc_m[0][0] = codeword[0];
  ldpc_e[82][173] = 0.0f;
  float tov[N][3];
  float toc[M][7];

  int min_errors = M;

  // initialize messages to checks
  for (int i = 0; i < M; ++i)
  {
    for (int j = 0; j < kNrw[i]; ++j)
    {
      toc[i][j] = codeword[kNm[i][j] - 1];
    }
  }

  for (int i = 0; i < N; ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      tov[i][j] = 0;
    }
  }

  for (int iter = 0; iter < max_iters; ++iter)
  {
    float zn[N];

    // Update bit log likelihood ratios (tov=0 in iter 0)
    for (int i = 0; i < N; ++i)
    {
      zn[i] = codeword[i] + tov[i][0] + tov[i][1] + tov[i][2];
      plain[i] = (zn[i] > 0) ? 1 : 0;
    }

    // Check to see if we have a codeword (check before we do any iter)
    int errors = ldpc_check(plain);

    if (errors < min_errors)
    {
      // we have a better guess - update the result
      min_errors = errors;

      if (errors == 0)
      {
        break; // Found a perfect answer
      }
    }

    // Send messages from bits to check nodes
    for (int i = 0; i < M; ++i)
    {
      for (int j = 0; j < kNrw[i]; ++j)
      {
        int ibj = kNm[i][j] - 1;
        toc[i][j] = zn[ibj];
        for (int kk = 0; kk < 3; ++kk)
        {
          // subtract off what the bit had received from the check
          if (kMn[ibj][kk] - 1 == i)
          {
            toc[i][j] -= tov[ibj][kk];
          }
        }
      }
    }

    // send messages from check nodes to variable nodes
    for (int i = 0; i < M; ++i)
    {
      for (int j = 0; j < kNrw[i]; ++j)
      {
        toc[i][j] = fast_tanh(-toc[i][j] / 2);
      }
    }

    for (int i = 0; i < N; ++i)
    {
      for (int j = 0; j < 3; ++j)
      {
        int ichk = kMn[i][j] - 1; // kMn(:,j) are the checks that include bit j
        float Tmn = 1.0f;
        for (int k = 0; k < kNrw[ichk]; ++k)
        {
          if (kNm[ichk][k] - 1 != i)
          {
            Tmn *= toc[ichk][k];
          }
        }
        tov[i][j] = 2 * fast_atanh(-Tmn);
      }
    }
  }

  *ok = min_errors;
}

// https://varietyofsound.wordpress.com/2011/02/14/efficient-tanh-computation-using-lamberts-continued-fraction/
// http://functions.wolfram.com/ElementaryFunctions/ArcTanh/10/0001/
// https://mathr.co.uk/blog/2017-09-06_approximating_hyperbolic_tangent.html

// thank you Douglas Bagnall
// https://math.stackexchange.com/a/446411
static float fast_tanh(float x)
{
  if (x < -4.97f)
  {
    return -1.0f;
  }
  if (x > 4.97f)
  {
    return 1.0f;
  }
  float x2 = x * x;
  float a = x * (945.0f + x2 * (105.0f + x2));
  float b = 945.0f + x2 * (420.0f + x2 * 15.0f);
  return a / b;
}

static float fast_atanh(float x)
{
  float x2 = x * x;
  float a = x * (945.0f + x2 * (-735.0f + x2 * 64.0f));
  float b = (945.0f + x2 * (-1050.0f + x2 * 225.0f));
  return a / b;
}
