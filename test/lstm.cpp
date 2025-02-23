/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include "driver.hpp"
#include "get_handle.hpp"
#include "tensor_holder.hpp"
#include "test.hpp"
#include "verify.hpp"
#include "rnn_util.hpp"
#include <array>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <miopen/rnn.hpp>
#include <miopen/miopen.h>
#include <miopen/tensor.hpp>
#include <utility>
#include <cfloat>

#define MIO_LSTM_TEST_DEBUG 0
#define MIO_RNN_TIME_EVERYTHING 0

/**********************************************
 * CPU verification functions
 *
 **********************************************/

template <typename T>
void LSTMFwdCPUVerify(std::vector<T>& in,
                      std::vector<T>& wei,     // [ input_state_weight_trans
                                               // hidden_state_weight0_trans input1_trans
                                               // hidden1_trans ... output_weight;
                                               // bidirectional reversed weights ]
                      std::vector<T>& hy_host, // current/final hidden state
                      std::vector<T>& hx,      // initial hidden state
                      std::vector<T>& cy_host, // current/final cell state
                      std::vector<T>& cx,      // initial cell state
                      std::vector<T>& out_host,
                      std::vector<int>& in_n, // input batch size
                      int in_h,               // input data length
                      int seqLength,          // Number of iterations to unroll over
                      int bidirection,        // whether using bidirectional net
                      int biased,             // whether using bias
                      int hy_d,  // 1 by numlayer (number of stacks of hidden layers) for
                                 // unidirection, 2 by numlayer for bidirection
                      int hy_n,  // equal to input batch size in_n[0]
                      int hy_h,  // hidden state number
                      int out_h, // 1 by hy_h related function for unidirection, 2 by hy_h
                                 // related function for bidirection
                      int inputMode,
                      std::vector<T>& rsvspace,
                      bool hx_is_null = false,
                      bool cx_is_null = false)
{
    int batch_n = sumvc(in_n);

    int numlayer = bidirection ? hy_d / 2 : hy_d;
    int bi       = bidirection ? 2 : 1;

    int in_stride  = in_h;
    int out_stride = out_h;
    int wei_stride = bi * 4 * hy_h;
    int hy_stride  = bi * 6 * hy_h;
    int h_stride   = bi * hy_h;
    int uni_stride = hy_h;
    int bi_stride  = hy_h * bi;

    if(inputMode == 1)
    {
        if(in_h != hy_h)
        {
            std::cout
                << "Verification cannot be completed: The input tensor size must equal to the "
                << "hidden state size of the network in SKIP_INPUT mode!" << std::endl;
            return;
        }
        in_h = 0;
    }

    int wei_shift_bias = (in_h + hy_h + (bi * hy_h + hy_h) * (numlayer - 1)) * wei_stride;
    int wei_len        = wei_shift_bias;
    if(biased)
    {
        int in_bias = 2;
        wei_len += (in_bias + (numlayer - 1) * 2) * wei_stride;
    }

    // forward emulator
    for(int li = 0; li < numlayer; li++)
    {
        int hid_shift = li * batch_n * hy_stride;
        int hx_shift  = li * in_n.at(0) * h_stride;

        // from input
        if(li == 0)
        {
            if(inputMode == 1)
            {
                for(int bs = 0; bs < batch_n; bs++)
                {
                    for(int h = 0; h < hy_h; h++)
                    {
                        for(int gi = 0; gi < 4; gi++)
                        {
                            rsvspace.at(hid_shift + bs * hy_stride + gi * hy_h + h) +=
                                in.at(bs * in_stride + h);
                            if(bidirection)
                            {
                                rsvspace.at(hid_shift + bs * hy_stride + (gi + 4) * hy_h + h) +=
                                    in.at(bs * in_stride + h);
                            }
                        }
                    }
                }

                // from bias
                if(biased)
                {
                    for(int bs = 0; bs < batch_n; bs++)
                    {
                        for(int h = 0; h < wei_stride; h++)
                        {
                            rsvspace.at(hid_shift + bs * hy_stride + h) +=
                                wei.at(wei_shift_bias + h);
                        }
                    }
                }
            }
            else
            {
                RNN_mm_cpu<T>(in.data(),
                              in_h,
                              batch_n,
                              in_stride,
                              0,
                              wei.data(),
                              in_h,
                              hy_h * bi * 4,
                              in_stride,
                              RNN_MM_TRANSPOSE,
                              &rsvspace[hid_shift],
                              hy_h * bi * 4,
                              batch_n,
                              hy_stride,
                              0,
                              1,
                              1);

                // from bias
                if(biased)
                {
                    for(int bs = 0; bs < batch_n; bs++)
                    {
                        for(int h = 0; h < wei_stride; h++)
                        {
                            rsvspace.at(hid_shift + bs * hy_stride + h) +=
                                wei.at(wei_shift_bias + h);
                        }
                    }
                }
            }
        }
        else
        {
            int wei_shift = (in_h + hy_h) * wei_stride + (li - 1) * (bi * hy_h + hy_h) * wei_stride;
            int prelayer_shift = (li - 1) * batch_n * hy_stride + bi * 5 * hy_h;

            RNN_mm_cpu<T>(&rsvspace[prelayer_shift],
                          hy_h * bi,
                          batch_n,
                          hy_stride,
                          0,
                          &wei[wei_shift],
                          hy_h * bi,
                          hy_h * bi * 4,
                          bi_stride,
                          RNN_MM_TRANSPOSE,
                          &rsvspace[hid_shift],
                          hy_h * bi * 4,
                          batch_n,
                          hy_stride,
                          0,
                          1,
                          1);

            // from bias
            if(biased)
            {
                int wei_shift_bias_temp = wei_shift_bias + li * 2 * wei_stride;

                for(int bs = 0; bs < batch_n; bs++)
                {
                    for(int h = 0; h < wei_stride; h++)
                    {
                        rsvspace.at(hid_shift + bs * hy_stride + h) +=
                            wei.at(wei_shift_bias_temp + h);
                    }
                }
            }
        }

        // from hidden state
        int bacc   = 0;
        int baccbi = batch_n;
        for(int ti = 0; ti < seqLength; ti++)
        {
            baccbi -= in_n.at(seqLength - 1 - ti);
            int wei_shift = in_h * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;

            if(ti == 0)
            {
                if(!hx_is_null)
                {
                    RNN_mm_cpu<T>(&hx[hx_shift],
                                  hy_h,
                                  in_n.at(ti),
                                  uni_stride,
                                  0,
                                  &wei[wei_shift],
                                  hy_h,
                                  hy_h * 4,
                                  uni_stride,
                                  RNN_MM_TRANSPOSE,
                                  &rsvspace[hid_shift + bacc * hy_stride],
                                  hy_h * 4,
                                  in_n.at(ti),
                                  hy_stride,
                                  0,
                                  1,
                                  1);

                    // from bias
                    if(biased)
                    {
                        int wei_shift_bias_temp = wei_shift_bias + (li * 2 + 1) * wei_stride;

                        for(int bs = 0; bs < in_n.at(ti); bs++)
                        {
                            for(int h = 0; h < 4 * hy_h; h++)
                            {
                                rsvspace.at(hid_shift + bacc * hy_stride + bs * hy_stride + h) +=
                                    wei.at(wei_shift_bias_temp + h);
                            }
                        }
                    }

                    if(bidirection)
                    {
                        RNN_mm_cpu<T>(&hx[hx_shift + hy_n * hy_h],
                                      hy_h,
                                      in_n.at(seqLength - 1 - ti),
                                      uni_stride,
                                      0,
                                      &wei[wei_shift + 4 * hy_h * uni_stride],
                                      hy_h,
                                      hy_h * 4,
                                      uni_stride,
                                      RNN_MM_TRANSPOSE,
                                      &rsvspace[hid_shift + baccbi * hy_stride + 4 * hy_h],
                                      hy_h * 4,
                                      in_n.at(seqLength - 1 - ti),
                                      hy_stride,
                                      0,
                                      1,
                                      1);

                        // from bias
                        if(biased)
                        {
                            int wei_shift_bias_temp = wei_shift_bias + (li * 2 + 1) * wei_stride;

                            for(int bs = 0; bs < in_n.at(seqLength - 1 - ti); bs++)
                            {
                                for(int h = 0; h < 4 * hy_h; h++)
                                {
                                    rsvspace.at(hid_shift + baccbi * hy_stride + 4 * hy_h +
                                                bs * hy_stride + h) +=
                                        wei.at(wei_shift_bias_temp + 4 * hy_h + h);
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                RNN_mm_cpu<T>(&hy_host[hx_shift],
                              hy_h,
                              in_n.at(ti),
                              uni_stride,
                              0,
                              &wei[wei_shift],
                              hy_h,
                              hy_h * 4,
                              uni_stride,
                              RNN_MM_TRANSPOSE,
                              &rsvspace[hid_shift + bacc * hy_stride],
                              hy_h * 4,
                              in_n.at(ti),
                              hy_stride,
                              0,
                              1,
                              1);

                // from bias
                if(biased)
                {
                    int wei_shift_bias_temp = wei_shift_bias + (li * 2 + 1) * wei_stride;

                    for(int bs = 0; bs < in_n.at(ti); bs++)
                    {
                        for(int h = 0; h < 4 * hy_h; h++)
                        {
                            rsvspace.at(hid_shift + bacc * hy_stride + bs * hy_stride + h) +=
                                wei.at(wei_shift_bias_temp + h);
                        }
                    }
                }

                if(bidirection)
                {

                    if(!hx_is_null && in_n.at(seqLength - 1 - ti) > in_n.at(seqLength - ti))
                    {
                        RNN_mm_cpu<T>(
                            &hx[hx_shift + hy_n * hy_h + in_n.at(seqLength - ti) * hy_h],
                            hy_h,
                            (in_n.at(seqLength - 1 - ti) - in_n.at(seqLength - ti)),
                            uni_stride,
                            0,
                            &wei[wei_shift + 4 * hy_h * uni_stride],
                            hy_h,
                            hy_h * 4,
                            uni_stride,
                            RNN_MM_TRANSPOSE,
                            &rsvspace[hid_shift + (baccbi + in_n.at(seqLength - ti)) * hy_stride +
                                      4 * hy_h],
                            hy_h * 4,
                            (in_n.at(seqLength - 1 - ti) - in_n.at(seqLength - ti)),
                            hy_stride,
                            0,
                            1,
                            1);

                        // from bias
                        if(biased)
                        {
                            int wei_shift_bias_temp = wei_shift_bias + (li * 2 + 1) * wei_stride;

                            for(int bs = in_n.at(seqLength - ti); bs < in_n.at(seqLength - 1 - ti);
                                bs++)
                            {
                                for(int h = 0; h < 4 * hy_h; h++)
                                {
                                    rsvspace.at(hid_shift + baccbi * hy_stride + 4 * hy_h +
                                                bs * hy_stride + h) +=
                                        wei.at(wei_shift_bias_temp + 4 * hy_h + h);
                                }
                            }
                        }
                    }

                    RNN_mm_cpu<T>(&hy_host[hx_shift + hy_n * hy_h],
                                  hy_h,
                                  in_n.at(seqLength - ti),
                                  uni_stride,
                                  0,
                                  &wei[wei_shift + 4 * hy_h * uni_stride],
                                  hy_h,
                                  hy_h * 4,
                                  uni_stride,
                                  RNN_MM_TRANSPOSE,
                                  &rsvspace[hid_shift + baccbi * hy_stride + 4 * hy_h],
                                  hy_h * 4,
                                  in_n.at(seqLength - ti),
                                  hy_stride,
                                  0,
                                  1,
                                  1);

                    // from bias
                    if(biased)
                    {
                        int wei_shift_bias_temp = wei_shift_bias + (li * 2 + 1) * wei_stride;

                        for(int bs = 0; bs < in_n.at(seqLength - ti); bs++)
                        {
                            for(int h = 0; h < 4 * hy_h; h++)
                            {
                                rsvspace.at(hid_shift + baccbi * hy_stride + 4 * hy_h +
                                            bs * hy_stride + h) +=
                                    wei.at(wei_shift_bias_temp + 4 * hy_h + h);
                            }
                        }
                    }
                }
            }

            for(int bs = 0; bs < in_n.at(ti); bs++)
            {
                for(int h = 0; h < hy_h; h++)
                {
                    rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h) +=
                        activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + h), 2) *
                        activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + 3 * hy_h + h),
                                  1);
                    if(ti == 0)
                    {
                        if(!cx_is_null)
                        {
                            rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h) +=
                                activfunc(
                                    rsvspace.at(hid_shift + (bacc + bs) * hy_stride + hy_h + h),
                                    2) *
                                cx.at(hx_shift + bs * uni_stride + h);
                        }
                    }
                    else
                    {
                        int prec_shift = li * batch_n * hy_stride +
                                         (bacc - in_n.at(ti - 1)) * hy_stride + bi * 4 * hy_h;

                        rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h) +=
                            activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + hy_h + h),
                                      2) *
                            rsvspace.at(prec_shift + bs * hy_stride + h);
                    }

                    rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 5 * hy_h + h) +=
                        activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + 2 * hy_h + h),
                                  2) *
                        activfunc(
                            rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h),
                            1);

                    rsvspace.at(hid_shift + (bacc + bs) * hy_stride + h +
                                numlayer * batch_n * hy_stride) =
                        activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + h), 2);
                    rsvspace.at(hid_shift + (bacc + bs) * hy_stride + hy_h + h +
                                numlayer * batch_n * hy_stride) =
                        activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + hy_h + h), 2);
                    rsvspace.at(hid_shift + (bacc + bs) * hy_stride + 2 * hy_h + h +
                                numlayer * batch_n * hy_stride) =
                        activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + 2 * hy_h + h),
                                  2);
                    rsvspace.at(hid_shift + (bacc + bs) * hy_stride + 3 * hy_h + h +
                                numlayer * batch_n * hy_stride) =
                        activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + 3 * hy_h + h),
                                  1);
                    rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h +
                                numlayer * batch_n * hy_stride) =
                        activfunc(
                            rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h),
                            1);

                    cy_host.at(hx_shift + bs * uni_stride + h) =
                        rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h);
                    hy_host.at(hx_shift + bs * uni_stride + h) =
                        rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 5 * hy_h + h);
                }
            }

            if(bidirection)
            {
                for(int bs = 0; bs < in_n.at(seqLength - 1 - ti); bs++)
                {
                    for(int h = 0; h < hy_h; h++)
                    {
                        rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 4 * hy_h + hy_h +
                                    h) +=
                            activfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 4 * hy_h + h),
                                2) *
                            activfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 7 * hy_h + h),
                                1);
                        if(ti == 0)
                        {
                            if(!cx_is_null)
                            {
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 4 * hy_h +
                                            hy_h + h) +=
                                    activfunc(rsvspace.at(hid_shift + (baccbi + bs) * hy_stride +
                                                          5 * hy_h + h),
                                              2) *
                                    cx.at(hx_shift + bs * uni_stride + hy_n * hy_h + h);
                            }
                        }
                        else
                        {

                            if(!cx_is_null && in_n.at(seqLength - 1 - ti) > in_n.at(seqLength - ti))
                            {
                                if(bs >= in_n.at(seqLength - ti))
                                {
                                    rsvspace.at(hid_shift + (baccbi + bs) * hy_stride +
                                                bi * 4 * hy_h + hy_h + h) +=
                                        activfunc(rsvspace.at(hid_shift +
                                                              (baccbi + bs) * hy_stride + 5 * hy_h +
                                                              h),
                                                  2) *
                                        cx.at(hx_shift + bs * uni_stride + hy_n * hy_h + h);
                                }
                            }

                            if(bs < in_n.at(seqLength - ti))
                            {
                                int prec_shift =
                                    li * batch_n * hy_stride +
                                    (baccbi + in_n.at(seqLength - 1 - ti)) * hy_stride +
                                    bi * 4 * hy_h + hy_h;

                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 4 * hy_h +
                                            hy_h + h) +=
                                    activfunc(rsvspace.at(hid_shift + (baccbi + bs) * hy_stride +
                                                          5 * hy_h + h),
                                              2) *
                                    rsvspace.at(prec_shift + bs * hy_stride + h);
                            }
                        }

                        rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 5 * hy_h + hy_h +
                                    h) +=
                            activfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 6 * hy_h + h),
                                2) *
                            activfunc(rsvspace.at(hid_shift + (baccbi + bs) * hy_stride +
                                                  bi * 4 * hy_h + hy_h + h),
                                      1);

                        rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 4 * hy_h + h +
                                    numlayer * batch_n * hy_stride) =
                            activfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 4 * hy_h + h),
                                2);
                        rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 5 * hy_h + h +
                                    numlayer * batch_n * hy_stride) =
                            activfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 5 * hy_h + h),
                                2);
                        rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 6 * hy_h + h +
                                    numlayer * batch_n * hy_stride) =
                            activfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 6 * hy_h + h),
                                2);
                        rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 7 * hy_h + h +
                                    numlayer * batch_n * hy_stride) =
                            activfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 7 * hy_h + h),
                                1);
                        rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 4 * hy_h + hy_h +
                                    h + numlayer * batch_n * hy_stride) =
                            activfunc(rsvspace.at(hid_shift + (baccbi + bs) * hy_stride +
                                                  bi * 4 * hy_h + hy_h + h),
                                      1);

                        cy_host.at(hx_shift + bs * uni_stride + hy_n * hy_h + h) = rsvspace.at(
                            hid_shift + (baccbi + bs) * hy_stride + bi * 4 * hy_h + hy_h + h);
                        hy_host.at(hx_shift + bs * uni_stride + hy_n * hy_h + h) = rsvspace.at(
                            hid_shift + (baccbi + bs) * hy_stride + bi * 5 * hy_h + hy_h + h);
                    }
                }
            }

            bacc += in_n.at(ti);
        }
    }

    // output
    int prelayer_shift = (numlayer - 1) * batch_n * hy_stride + bi * 5 * hy_h;

    for(int bs = 0; bs < batch_n; bs++)
    {
        for(int h = 0; h < out_h; h++)
        {
            out_host.at(bs * out_stride + h) = rsvspace.at(prelayer_shift + bs * hy_stride + h);
        }
    }
}

template <typename T>
void LSTMBwdDataCPUVerify(std::vector<T>& din_host,
                          std::vector<T>& wei, // [ input_state_weight_trans
                                               // hidden_state_weight0_trans input1_trans
                                               // hidden1_trans ... output_weight;
                                               // bidirectional reversed weights ]
                          std::vector<T>& dhy, // current/final hidden state
                          std::vector<T>& dhx_host,
                          std::vector<T>& hx,  // initial hidden state
                          std::vector<T>& dcy, // current/final cell state
                          std::vector<T>& dcx_host,
                          std::vector<T>& cx,
                          std::vector<T>& out,
                          std::vector<T>& dout,
                          std::vector<int>& in_n, // input batch size
                          int in_h,               // input data length
                          int seqLength,          // Number of iterations to unroll over
                          int bidirection,        // whether using bidirectional net
                          int biased,             // whether using bias
                          int hy_d,  // 1 by numlayer (number of stacks of hidden layers)
                                     // for unidirection, 2 by numlayer for bidirection
                          int hy_n,  // equal to input batch size in_n[0]
                          int hy_h,  // hidden state number
                          int out_h, // 1 by hy_h related function for unidirection, 2 by
                                     // hy_h related function for bidirection
                          int inputMode,
                          std::vector<T>& rsvspace,
                          std::vector<T>& wkspace,
                          bool cx_is_null  = false,
                          bool dhy_is_null = false,
                          bool dcy_is_null = false)
{
    int batch_n = sumvc(in_n);
    (void)out;
    (void)hx;

    int numlayer = bidirection ? hy_d / 2 : hy_d;
    int bi       = bidirection ? 2 : 1;

    int in_stride  = in_h;
    int out_stride = out_h;
    int wei_stride = bi * 4 * hy_h;
    int hy_stride  = bi * 6 * hy_h;
    int h_stride   = bi * hy_h;
    int uni_stride = hy_h;
    int bi_stride  = hy_h * bi;

    if(inputMode == 1)
    {
        if(in_h != hy_h)
        {
            std::cout
                << "Verification cannot be completed: The input tensor size must equal to the "
                << "hidden state size of the network in SKIP_INPUT mode!" << std::endl;
            return;
        }
        in_h = 0;
    }

    int wei_len = (in_h + hy_h + (bi * hy_h + hy_h) * (numlayer - 1)) * wei_stride;
    if(biased)
    {
        int in_bias = 2;
        wei_len += (in_bias + (numlayer - 1) * 2) * wei_stride;
    }

    // bwd data emulator
    for(int li = numlayer - 1; li >= 0; li--)
    {
        int wei_shift = (in_h + hy_h) * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;
        int hid_shift = li * batch_n * hy_stride;
        int hx_shift  = li * in_n.at(0) * h_stride;

        if(li == numlayer - 1)
        {
            for(int bs = 0; bs < batch_n; bs++)
            {
                for(int h = 0; h < out_h; h++)
                {
                    wkspace.at(hid_shift + bi * 5 * hy_h + bs * hy_stride + h) +=
                        dout.at(bs * out_stride + h);
                }
            }
        }
        else
        {
            int prelayer_shift = (li + 1) * batch_n * hy_stride;

            RNN_mm_cpu<T>(&wkspace[prelayer_shift],
                          hy_h * bi * 4,
                          batch_n,
                          hy_stride,
                          0,
                          &wei[wei_shift],
                          hy_h * bi,
                          hy_h * bi * 4,
                          bi_stride,
                          0,
                          &wkspace[hid_shift + bi * 5 * hy_h],
                          hy_h * bi,
                          batch_n,
                          hy_stride,
                          0,
                          1,
                          1);
        }

        // from hidden state
        int bacc   = batch_n;
        int baccbi = 0;
        for(int ti = seqLength - 1; ti >= 0; ti--)
        {
            bacc -= in_n.at(ti);

            if(ti == seqLength - 1)
            {
                for(int bs = 0; bs < in_n.at(ti); bs++)
                {
                    for(int h = 0; h < hy_h; h++)
                    {
                        if(!dhy_is_null)
                        {
                            wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 5 * hy_h + h) +=
                                dhy.at(hx_shift + bs * uni_stride + h);
                        }
                        if(!dcy_is_null)
                        {
                            wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h) +=
                                dcy.at(hx_shift + bs * uni_stride + h);
                        }
                    }
                }

                if(bidirection)
                {
                    for(int bs = 0; bs < in_n.at(seqLength - 1 - ti); bs++)
                    {
                        for(int h = 0; h < hy_h; h++)
                        {
                            if(!dhy_is_null)
                            {
                                wkspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 5 * hy_h +
                                           hy_h + h) +=
                                    dhy.at(hx_shift + bs * uni_stride + hy_n * hy_h + h);
                            }
                            if(!dcy_is_null)
                            {
                                wkspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 4 * hy_h +
                                           hy_h + h) +=
                                    dcy.at(hx_shift + bs * uni_stride + hy_n * hy_h + h);
                            }
                        }
                    }
                }
            }
            else
            {
                if(!dhy_is_null && in_n.at(ti) > in_n.at(ti + 1))
                {
                    for(int bs = in_n.at(ti + 1); bs < in_n.at(ti); bs++)
                    {
                        for(int h = 0; h < hy_h; h++)
                        {
                            wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 5 * hy_h + h) +=
                                dhy.at(hx_shift + bs * uni_stride + h);
                        }
                    }
                }

                if(!dcy_is_null && in_n.at(ti) > in_n.at(ti + 1))
                {
                    for(int bs = in_n.at(ti + 1); bs < in_n.at(ti); bs++)
                    {
                        for(int h = 0; h < hy_h; h++)
                        {
                            wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h) +=
                                dcy.at(hx_shift + bs * uni_stride + h);
                        }
                    }
                }

                int pretime_shift = li * batch_n * hy_stride + (bacc + in_n.at(ti)) * hy_stride;
                int weitime_shift = in_h * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;

                RNN_mm_cpu<T>(&wkspace[pretime_shift],
                              hy_h * 4,
                              in_n.at(ti + 1),
                              hy_stride,
                              0,
                              &wei[weitime_shift],
                              hy_h,
                              hy_h * 4,
                              uni_stride,
                              0,
                              &wkspace[hid_shift + bacc * hy_stride + bi * 5 * hy_h],
                              hy_h,
                              in_n.at(ti + 1),
                              hy_stride,
                              0,
                              1,
                              1);

                if(bidirection)
                {
                    pretime_shift = li * batch_n * hy_stride +
                                    (baccbi - in_n.at(seqLength - 2 - ti)) * hy_stride + hy_h * 4;
                    weitime_shift = in_h * wei_stride + li * (bi * hy_h + hy_h) * wei_stride +
                                    hy_h * 4 * uni_stride;

                    RNN_mm_cpu<T>(&wkspace[pretime_shift],
                                  hy_h * 4,
                                  in_n.at(seqLength - 1 - ti),
                                  hy_stride,
                                  0,
                                  &wei[weitime_shift],
                                  hy_h,
                                  hy_h * 4,
                                  uni_stride,
                                  0,
                                  &wkspace[hid_shift + baccbi * hy_stride + bi * 5 * hy_h + hy_h],
                                  hy_h,
                                  in_n.at(seqLength - 1 - ti),
                                  hy_stride,
                                  0,
                                  1,
                                  1);
                }
            }

            for(int bs = 0; bs < in_n.at(ti); bs++)
            {
                for(int h = 0; h < hy_h; h++)
                {
                    if(ti < seqLength - 1)
                    {
                        if(bs < in_n.at(ti + 1))
                        {
                            int pretime_shift =
                                li * batch_n * hy_stride + (bacc + in_n.at(ti)) * hy_stride;

                            wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h) +=
                                wkspace.at(pretime_shift + bs * hy_stride + bi * 4 * hy_h + h) *
                                activfunc(rsvspace.at(pretime_shift + bs * hy_stride + hy_h + h),
                                          2);
                        }
                    }
                    wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h) +=
                        wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 5 * hy_h + h) *
                        dervactivfunc(
                            rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h),
                            1) *
                        activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + 2 * hy_h + h),
                                  2);

                    if(ti == 0)
                    {
                        if(!cx_is_null)
                        {
                            wkspace.at(hid_shift + (bacc + bs) * hy_stride + hy_h + h) +=
                                wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h +
                                           h) *
                                cx.at(hx_shift + bs * uni_stride + h) *
                                dervactivfunc(
                                    rsvspace.at(hid_shift + (bacc + bs) * hy_stride + hy_h + h), 2);
                        }
                    }
                    else
                    {
                        int pretime_shift =
                            li * batch_n * hy_stride + (bacc - in_n.at(ti - 1)) * hy_stride;

                        wkspace.at(hid_shift + (bacc + bs) * hy_stride + hy_h + h) +=
                            wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h) *
                            rsvspace.at(pretime_shift + bs * hy_stride + bi * 4 * hy_h + h) *
                            dervactivfunc(
                                rsvspace.at(hid_shift + (bacc + bs) * hy_stride + hy_h + h), 2);
                    }
                    wkspace.at(hid_shift + (bacc + bs) * hy_stride + h) +=
                        wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h) *
                        activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + 3 * hy_h + h),
                                  1) *
                        dervactivfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + h), 2);
                    wkspace.at(hid_shift + (bacc + bs) * hy_stride + 2 * hy_h + h) +=
                        wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 5 * hy_h + h) *
                        activfunc(
                            rsvspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h),
                            1) *
                        dervactivfunc(
                            rsvspace.at(hid_shift + (bacc + bs) * hy_stride + 2 * hy_h + h), 2);
                    wkspace.at(hid_shift + (bacc + bs) * hy_stride + 3 * hy_h + h) +=
                        wkspace.at(hid_shift + (bacc + bs) * hy_stride + bi * 4 * hy_h + h) *
                        activfunc(rsvspace.at(hid_shift + (bacc + bs) * hy_stride + h), 2) *
                        dervactivfunc(
                            rsvspace.at(hid_shift + (bacc + bs) * hy_stride + 3 * hy_h + h), 1);
                }
            }

            if(bidirection)
            {
                for(int bs = 0; bs < in_n.at(seqLength - 1 - ti); bs++)
                {
                    for(int h = 0; h < hy_h; h++)
                    {
                        if(ti < seqLength - 1)
                        {
                            int pretime_shift = li * batch_n * hy_stride +
                                                (baccbi - in_n.at(seqLength - 2 - ti)) * hy_stride;

                            wkspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 4 * hy_h +
                                       hy_h + h) +=
                                wkspace.at(pretime_shift + bs * hy_stride + bi * 4 * hy_h + hy_h +
                                           h) *
                                activfunc(
                                    rsvspace.at(pretime_shift + bs * hy_stride + 5 * hy_h + h), 2);
                        }
                        wkspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 4 * hy_h + hy_h +
                                   h) +=
                            wkspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 5 * hy_h +
                                       hy_h + h) *
                            dervactivfunc(rsvspace.at(hid_shift + (baccbi + bs) * hy_stride +
                                                      bi * 4 * hy_h + hy_h + h),
                                          1) *
                            activfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 6 * hy_h + h),
                                2);

                        if(ti == 0)
                        {
                            if(!cx_is_null)
                            {
                                wkspace.at(hid_shift + (baccbi + bs) * hy_stride + 5 * hy_h + h) +=
                                    wkspace.at(hid_shift + (baccbi + bs) * hy_stride +
                                               bi * 4 * hy_h + hy_h + h) *
                                    cx.at(hx_shift + bs * uni_stride + hy_n * hy_h + h) *
                                    dervactivfunc(rsvspace.at(hid_shift +
                                                              (baccbi + bs) * hy_stride + 5 * hy_h +
                                                              h),
                                                  2);
                            }
                        }
                        else
                        {
                            if(!cx_is_null &&
                               in_n.at(seqLength - 1 - ti) > in_n.at(seqLength - ti) &&
                               bs >= in_n.at(seqLength - ti))
                            {
                                wkspace.at(hid_shift + (baccbi + bs) * hy_stride + 5 * hy_h + h) +=
                                    wkspace.at(hid_shift + (baccbi + bs) * hy_stride +
                                               bi * 4 * hy_h + hy_h + h) *
                                    cx.at(hx_shift + bs * uni_stride + hy_n * hy_h + h) *
                                    dervactivfunc(rsvspace.at(hid_shift +
                                                              (baccbi + bs) * hy_stride + 5 * hy_h +
                                                              h),
                                                  2);
                            }

                            if(bs < in_n.at(seqLength - ti))
                            {
                                int pretime_shift =
                                    li * batch_n * hy_stride +
                                    (baccbi + in_n.at(seqLength - 1 - ti)) * hy_stride;

                                wkspace.at(hid_shift + (baccbi + bs) * hy_stride + 5 * hy_h + h) +=
                                    wkspace.at(hid_shift + (baccbi + bs) * hy_stride +
                                               bi * 4 * hy_h + hy_h + h) *
                                    rsvspace.at(pretime_shift + bs * hy_stride + bi * 4 * hy_h +
                                                hy_h + h) *
                                    dervactivfunc(rsvspace.at(hid_shift +
                                                              (baccbi + bs) * hy_stride + 5 * hy_h +
                                                              h),
                                                  2);
                            }
                        }
                        wkspace.at(hid_shift + (baccbi + bs) * hy_stride + 4 * hy_h + h) +=
                            wkspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 4 * hy_h +
                                       hy_h + h) *
                            activfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 7 * hy_h + h),
                                1) *
                            dervactivfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 4 * hy_h + h),
                                2);
                        wkspace.at(hid_shift + (baccbi + bs) * hy_stride + 6 * hy_h + h) +=
                            wkspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 5 * hy_h +
                                       hy_h + h) *
                            activfunc(rsvspace.at(hid_shift + (baccbi + bs) * hy_stride +
                                                  bi * 4 * hy_h + hy_h + h),
                                      1) *
                            dervactivfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 6 * hy_h + h),
                                2);
                        wkspace.at(hid_shift + (baccbi + bs) * hy_stride + 7 * hy_h + h) +=
                            wkspace.at(hid_shift + (baccbi + bs) * hy_stride + bi * 4 * hy_h +
                                       hy_h + h) *
                            activfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 4 * hy_h + h),
                                2) *
                            dervactivfunc(
                                rsvspace.at(hid_shift + (baccbi + bs) * hy_stride + 7 * hy_h + h),
                                1);
                    }
                }
            }

            baccbi += in_n.at(seqLength - 1 - ti);
        }

        // dcx, dhx
        int pretime_shift = li * batch_n * hy_stride;
        int weitime_shift = in_h * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;

        RNN_mm_cpu<T>(&wkspace[pretime_shift],
                      hy_h * 4,
                      in_n.at(0),
                      hy_stride,
                      0,
                      &wei[weitime_shift],
                      hy_h,
                      hy_h * 4,
                      uni_stride,
                      0,
                      &dhx_host[hx_shift],
                      hy_h,
                      in_n.at(0),
                      uni_stride,
                      0,
                      1,
                      1);

        for(int bs = 0; bs < in_n.at(0); bs++)
        {
            for(int h = 0; h < hy_h; h++)
            {
                dcx_host.at(hx_shift + bs * uni_stride + h) +=
                    wkspace.at(pretime_shift + bs * hy_stride + bi * 4 * hy_h + h) *
                    activfunc(rsvspace.at(pretime_shift + bs * hy_stride + hy_h + h), 2);
            }
        }

        if(bidirection)
        {
            int ti = seqLength - 1, cur_bat = 0, pre_bat = batch_n;

            while(ti >= 0)
            {
                pre_bat -= in_n.at(ti);
                if(in_n.at(ti) > cur_bat)
                {
                    pretime_shift = li * batch_n * hy_stride + (pre_bat + cur_bat) * hy_stride;

                    RNN_mm_cpu<T>(&wkspace[pretime_shift + 4 * hy_h],
                                  hy_h * 4,
                                  (in_n.at(ti) - cur_bat),
                                  hy_stride,
                                  0,
                                  &wei[weitime_shift + 4 * hy_h * uni_stride],
                                  hy_h,
                                  hy_h * 4,
                                  uni_stride,
                                  0,
                                  &dhx_host[hx_shift + hy_n * hy_h + cur_bat * hy_h],
                                  hy_h,
                                  (in_n.at(ti) - cur_bat),
                                  uni_stride,
                                  0,
                                  1,
                                  1);

                    for(int bs = cur_bat; bs < in_n.at(ti); bs++)
                    {
                        for(int h = 0; h < hy_h; h++)
                        {
                            dcx_host.at(hx_shift + bs * uni_stride + hy_n * hy_h + h) +=
                                wkspace.at(pretime_shift + (bs - cur_bat) * hy_stride +
                                           bi * 4 * hy_h + hy_h + h) *
                                activfunc(rsvspace.at(pretime_shift + (bs - cur_bat) * hy_stride +
                                                      5 * hy_h + h),
                                          2);
                        }
                    }
                }
                cur_bat = in_n.at(ti--);
            }
        }
    }

    // dinput
    if(inputMode == 1)
    {
        for(int bs = 0; bs < batch_n; bs++)
        {
            for(int h = 0; h < hy_h; h++)
            {
                for(int gi = 0; gi < 4; gi++)
                {
                    din_host.at(bs * in_stride + h) += wkspace.at(bs * hy_stride + gi * hy_h + h);
                    if(bidirection)
                    {
                        din_host.at(bs * in_stride + h) +=
                            wkspace.at(bs * hy_stride + (gi + 4) * hy_h + h);
                    }
                }
            }
        }
    }
    else
    {
        RNN_mm_cpu<T>(wkspace.data(),
                      hy_h * bi * 4,
                      batch_n,
                      hy_stride,
                      0,
                      wei.data(),
                      in_h,
                      hy_h * bi * 4,
                      in_stride,
                      0,
                      din_host.data(),
                      in_h,
                      batch_n,
                      in_stride,
                      0,
                      1,
                      1);
    }
}

template <typename T>
void LSTMBwdWeightCPUVerify(std::vector<T>& in,
                            std::vector<T>& dwei_host, // [ input_state_weight_trans
                                                       // hidden_state_weight0_trans
                                                       // input1_trans hidden1_trans ...
                                                       // output_weight; bidirectional
                                                       // reversed weights ]
                            std::vector<T>& hx,        // initial hidden state
                            std::vector<T>& dout,
                            std::vector<int>& in_n, // input batch size
                            int in_h,               // input data length
                            int seqLength,          // Number of iterations to unroll over
                            int bidirection,        // whether using bidirectional net
                            int biased,             // whether using bias
                            int hy_d,               // 1 by numlayer (number of stacks of hidden
                                                    // layers) for unidirection, 2 by numlayer for
                                                    // bidirection
                            int hy_n,               // equal to input batch size in_n[0]
                            int hy_h,               // hidden state number
                            int out_h, // 1 by hy_h related function for unidirection, 2
                                       // by hy_h related function for bidirection
                            int inputMode,
                            std::vector<T>& rsvspace,
                            std::vector<T>& wkspace,
                            bool hx_is_null = false)
{
    int batch_n  = sumvc(in_n);
    int numlayer = bidirection ? hy_d / 2 : hy_d;
    int bi       = bidirection ? 2 : 1;

    int in_stride  = in_h;
    int wei_stride = bi * 4 * hy_h;
    int hy_stride  = bi * 6 * hy_h;
    int h_stride   = bi * hy_h;
    int uni_stride = hy_h;
    int bi_stride  = hy_h * bi;
    (void)dout;
    (void)out_h;

    if(inputMode == 1)
    {
        if(in_h != hy_h)
        {
            std::cout
                << "Verification cannot be completed: The input tensor size must equal to the "
                << "hidden state size of the network in SKIP_INPUT mode!" << std::endl;
            return;
        }
        in_h = 0;
    }

    int wei_shift_bias = (in_h + hy_h + (bi * hy_h + hy_h) * (numlayer - 1)) * wei_stride;
    int wei_len        = wei_shift_bias;
    if(biased)
    {
        int in_bias = 2;
        wei_len += (in_bias + (numlayer - 1) * 2) * wei_stride;
    }

    // bwd weights emulator
    for(int li = 0; li < numlayer; li++)
    {
        // between layers
        if(li == 0)
        {
            if(inputMode != 1)
            {
                RNN_mm_cpu<T>(wkspace.data(),
                              hy_h * bi * 4,
                              batch_n,
                              hy_stride,
                              RNN_MM_TRANSPOSE,
                              in.data(),
                              in_h,
                              batch_n,
                              in_stride,
                              0,
                              dwei_host.data(),
                              in_h,
                              hy_h * bi * 4,
                              in_stride,
                              0,
                              1,
                              1);
            }

            if(biased)
            {
                for(int h = 0; h < wei_stride; h++)
                {
                    for(int w = 0; w < batch_n; w++)
                    {
                        dwei_host.at(wei_shift_bias + h) += wkspace.at(w * hy_stride + h);
                    }
                }
            }
        }
        else
        {
            int prelayer_shift = (li - 1) * batch_n * hy_stride + bi * hy_h * 5;
            int hid_shift      = li * batch_n * hy_stride;
            int wei_shift = (in_h + hy_h) * wei_stride + (li - 1) * (bi * hy_h + hy_h) * wei_stride;

            RNN_mm_cpu<T>(&wkspace[hid_shift],
                          hy_h * bi * 4,
                          batch_n,
                          hy_stride,
                          RNN_MM_TRANSPOSE,
                          &rsvspace[prelayer_shift],
                          hy_h * bi,
                          batch_n,
                          hy_stride,
                          0,
                          &dwei_host[wei_shift],
                          hy_h * bi,
                          hy_h * bi * 4,
                          bi_stride,
                          0,
                          1,
                          1);

            if(biased)
            {
                wei_shift = wei_shift_bias + li * 2 * wei_stride;

                for(int h = 0; h < wei_stride; h++)
                {
                    for(int w = 0; w < batch_n; w++)
                    {
                        dwei_host.at(wei_shift + h) += wkspace.at(hid_shift + w * hy_stride + h);
                    }
                }
            }
        }

        // between time
        int bacc = 0;
        for(int ti = 0; ti < seqLength; ti++)
        {
            int hid_shift = li * batch_n * hy_stride + bacc * hy_stride;
            int hx_shift  = li * in_n.at(0) * h_stride;
            int wei_shift = in_h * wei_stride + li * (bi * hy_h + hy_h) * wei_stride;
            int pretime_shift;

            // between time
            if(ti == 0)
            {
                if(!hx_is_null)
                {
                    RNN_mm_cpu<T>(&wkspace[hid_shift],
                                  hy_h * 4,
                                  in_n.at(ti),
                                  hy_stride,
                                  RNN_MM_TRANSPOSE,
                                  &hx[hx_shift],
                                  hy_h,
                                  in_n.at(ti),
                                  uni_stride,
                                  0,
                                  &dwei_host[wei_shift],
                                  hy_h,
                                  hy_h * 4,
                                  uni_stride,
                                  0,
                                  1,
                                  1);

                    if(biased)
                    {
                        int bias_shift = wei_shift_bias + li * 2 * wei_stride + wei_stride;

                        for(int h = 0; h < hy_h * 4; h++)
                        {
                            for(int w = 0; w < in_n.at(ti); w++)
                            {
                                dwei_host.at(bias_shift + h) +=
                                    wkspace.at(hid_shift + w * hy_stride + h);
                            }
                        }
                    }
                }
            }
            else
            {
                pretime_shift =
                    li * batch_n * hy_stride + (bacc - in_n.at(ti - 1)) * hy_stride + bi * 5 * hy_h;

                RNN_mm_cpu<T>(&wkspace[hid_shift],
                              hy_h * 4,
                              in_n.at(ti),
                              hy_stride,
                              RNN_MM_TRANSPOSE,
                              &rsvspace[pretime_shift],
                              hy_h,
                              in_n.at(ti),
                              hy_stride,
                              0,
                              &dwei_host[wei_shift],
                              hy_h,
                              hy_h * 4,
                              uni_stride,
                              0,
                              1,
                              1);

                if(biased)
                {
                    int bias_shift = wei_shift_bias + li * 2 * wei_stride + wei_stride;

                    for(int h = 0; h < hy_h * 4; h++)
                    {
                        for(int w = 0; w < in_n.at(ti); w++)
                        {
                            dwei_host.at(bias_shift + h) +=
                                wkspace.at(hid_shift + w * hy_stride + h);
                        }
                    }
                }
            }

            if(bidirection)
            {
                if(ti == seqLength - 1)
                {
                    if(!hx_is_null)
                    {
                        RNN_mm_cpu<T>(&wkspace[hid_shift + 4 * hy_h],
                                      hy_h * 4,
                                      in_n.at(ti),
                                      hy_stride,
                                      RNN_MM_TRANSPOSE,
                                      &hx[hx_shift + hy_n * hy_h],
                                      hy_h,
                                      in_n.at(ti),
                                      uni_stride,
                                      0,
                                      &dwei_host[wei_shift + 4 * hy_h * uni_stride],
                                      hy_h,
                                      hy_h * 4,
                                      uni_stride,
                                      0,
                                      1,
                                      1);

                        if(biased)
                        {
                            int bias_shift = wei_shift_bias + li * 2 * wei_stride + wei_stride;

                            for(int h = 0; h < hy_h * 4; h++)
                            {
                                for(int w = 0; w < in_n.at(ti); w++)
                                {
                                    dwei_host.at(bias_shift + hy_h * 4 + h) +=
                                        wkspace.at(hid_shift + hy_h * 4 + w * hy_stride + h);
                                }
                            }
                        }
                    }
                }
                else
                {
                    if(!hx_is_null && in_n.at(ti) > in_n.at(ti + 1))
                    {
                        RNN_mm_cpu<T>(&wkspace[hid_shift + 4 * hy_h + in_n.at(ti + 1) * hy_stride],
                                      hy_h * 4,
                                      (in_n.at(ti) - in_n.at(ti + 1)),
                                      hy_stride,
                                      RNN_MM_TRANSPOSE,
                                      &hx[hx_shift + hy_n * hy_h + in_n.at(ti + 1) * hy_h],
                                      hy_h,
                                      (in_n.at(ti) - in_n.at(ti + 1)),
                                      uni_stride,
                                      0,
                                      &dwei_host[wei_shift + 4 * hy_h * uni_stride],
                                      hy_h,
                                      hy_h * 4,
                                      uni_stride,
                                      0,
                                      1,
                                      1);

                        if(biased)
                        {
                            int bias_shift = wei_shift_bias + li * 2 * wei_stride + wei_stride;

                            for(int h = 0; h < hy_h * 4; h++)
                            {
                                for(int w = in_n.at(ti + 1); w < in_n.at(ti); w++)
                                {
                                    dwei_host.at(bias_shift + hy_h * 4 + h) +=
                                        wkspace.at(hid_shift + hy_h * 4 + w * hy_stride + h);
                                }
                            }
                        }
                    }

                    pretime_shift =
                        li * batch_n * hy_stride + (bacc + in_n.at(ti)) * hy_stride + bi * 5 * hy_h;

                    RNN_mm_cpu<T>(&wkspace[hid_shift + 4 * hy_h],
                                  hy_h * 4,
                                  in_n.at(ti + 1),
                                  hy_stride,
                                  RNN_MM_TRANSPOSE,
                                  &rsvspace[pretime_shift + hy_h],
                                  hy_h,
                                  in_n.at(ti + 1),
                                  hy_stride,
                                  0,
                                  &dwei_host[wei_shift + 4 * hy_h * uni_stride],
                                  hy_h,
                                  hy_h * 4,
                                  uni_stride,
                                  0,
                                  1,
                                  1);

                    if(biased)
                    {
                        int bias_shift = wei_shift_bias + li * 2 * wei_stride + wei_stride;

                        for(int h = 0; h < hy_h * 4; h++)
                        {
                            for(int w = 0; w < in_n.at(ti + 1); w++)
                            {
                                dwei_host.at(bias_shift + hy_h * 4 + h) +=
                                    wkspace.at(hid_shift + hy_h * 4 + w * hy_stride + h);
                            }
                        }
                    }
                }
            }

            bacc += in_n.at(ti);
        }
    }
}
//////=========END CPU VERIFICATION FUNCTIONS=============

//****************************************************
// FORWARD INFERENCE
//****************************************************
template <class T>
struct verify_forward_infer_lstm
{
    std::vector<T> input;
    std::vector<T> initHidden;
    std::vector<T> initCell;
    std::vector<T> weights;
    std::vector<int> batch_seq;
    int hiddenSize;
    int seqLength;
    int nLayers;
    int biasMode;
    int dirMode;
    int inputMode;
    int batch_n;
    int inputVecLen;
    miopenRNNDescriptor_t rnnDesc;
    size_t realHiddenSize;
    bool nohx;
    bool nocx;
    bool nohy;
    bool nocy;

    verify_forward_infer_lstm(miopenRNNDescriptor_t pRD,
                              const std::vector<T>& px,
                              const std::vector<T>& phx,
                              const std::vector<T>& pcx,
                              const std::vector<T>& pW,
                              const std::vector<int>& pBS,
                              const int pHS,
                              const int pBN,
                              const int pS,
                              const int pNL,
                              const int pBM,
                              const int pDM,
                              const int pIM,
                              const int pVL,
                              const size_t pHXZ,
                              const bool pnohx = false,
                              const bool pnocx = false,
                              const bool pnohy = false,
                              const bool pnocy = false)
    {
        rnnDesc = pRD;
        input   = px;
        weights = pW, batch_seq = pBS;
        seqLength      = pS;
        nLayers        = pNL;
        biasMode       = pBM;
        dirMode        = pDM;
        inputMode      = pIM;
        batch_n        = pBN;
        hiddenSize     = pHS;
        inputVecLen    = pVL;
        realHiddenSize = pHXZ;

        nohy = pnohy;
        nocy = pnocy;

        nohx = pnohx;
        if(!nohx)
            initHidden = phx; // this may be intentionally a nullptr
        else
            initHidden.resize(realHiddenSize);

        nocx = pnocx;
        if(!nocx)
            initCell = pcx; // this may be intentionally a nullptr
        else
            initCell.resize(realHiddenSize);
    }

    std::vector<T> cpu()
    {

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_start = std::chrono::high_resolution_clock::now();
#endif

        auto&& handle = get_handle();

        int bi        = dirMode ? 2 : 1;
        int hy_h      = hiddenSize;
        int bi_stride = bi * hy_h;
        size_t out_sz = 0;

        size_t reserveSpaceSize;

        std::vector<miopen::TensorDescriptor> inputCPPDescs;
        std::vector<miopenTensorDescriptor_t> inputDescs;
        createTensorDescArray(
            inputCPPDescs, inputDescs, batch_seq, inputVecLen, miopen::deref(rnnDesc).dataType);

        std::vector<miopen::TensorDescriptor> outputCPPDescs;
        std::vector<miopenTensorDescriptor_t> outputDescs;
        createTensorDescArray(outputCPPDescs,
                              outputDescs,
                              batch_seq,
                              hiddenSize * ((dirMode) ? 2 : 1),
                              miopen::deref(rnnDesc).dataType);
        miopenGetRNNInputTensorSize(&handle, rnnDesc, seqLength, outputDescs.data(), &out_sz);
        miopenGetRNNTrainingReserveSize(
            &handle, rnnDesc, seqLength, inputDescs.data(), &reserveSpaceSize);
        std::vector<T> reserveSpace(reserveSpaceSize / sizeof(T));
        std::vector<T> output(out_sz / sizeof(T));
        std::vector<T> hiddenState(initHidden.size());
        std::vector<T> cellState(initCell.size());

        LSTMFwdCPUVerify(input,
                         weights,     // [ input_state_weight_trans
                                      // hidden_state_weight0_trans input1_trans
                                      // hidden1_trans ... output_weight;
                                      // bidirectional reversed weights ]
                         hiddenState, // current/final hidden state
                         initHidden,  // initial hidden state
                         cellState,   // current/final cell state
                         initCell,    // initial cell state
                         output,
                         batch_seq,       // input batch size
                         inputVecLen,     // input data length
                         seqLength,       // Number of iterations to unroll over
                         dirMode,         // whether using bidirectional net
                         biasMode,        // whether using bias
                         bi * nLayers,    // 1 by numlayer (number of stacks of hidden layers) for
                                          // unidirection, 2 by numlayer for bidirection
                         batch_seq.at(0), // equal to input batch size in_n[0]
                         hiddenSize,      // hidden state number
                         bi_stride,       // 1 by hy_h related function for unidirection, 2 by hy_h
                                          // related function for bidirection
                         inputMode,
                         reserveSpace,
                         nohx,
                         nocx);

#if(MIO_LSTM_TEST_DEBUG == 2)
        for(int i = 0; i < output.size(); i++)
        {
            printf("CPU outdata[%d]: %f\n", i, output[i]);
        }
#endif

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_end = std::chrono::high_resolution_clock::now();

        std::cout << "Wall clock: CPU forward inference LSTM pass time: "
                  << std::chrono::duration<double>(t_end - t_start).count() << " seconds."
                  << std::endl;
#endif
        auto retSet = std::make_tuple(output, hiddenState, weights, reserveSpace);
#if(MIO_LSTM_TEST_DEBUG > 0)
        std::cout << "Done with LSTM forward inference CPU" << std::endl;
        std::cout << "---------------------------------\n" << std::endl;
#endif
        return output;
    }

    std::vector<T> gpu()
    {

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_start = std::chrono::high_resolution_clock::now();
#endif

        auto&& handle = get_handle();

        size_t out_sz        = 0;
        size_t workSpaceSize = 0;

        std::vector<miopen::TensorDescriptor> inputCPPDescs;
        std::vector<miopenTensorDescriptor_t> inputDescs;
        createTensorDescArray(
            inputCPPDescs, inputDescs, batch_seq, inputVecLen, miopen::deref(rnnDesc).dataType);

        std::vector<miopen::TensorDescriptor> outputCPPDescs;
        std::vector<miopenTensorDescriptor_t> outputDescs;
        createTensorDescArray(outputCPPDescs,
                              outputDescs,
                              batch_seq,
                              hiddenSize * ((dirMode) ? 2 : 1),
                              miopen::deref(rnnDesc).dataType);

        miopenGetRNNWorkspaceSize(&handle, rnnDesc, seqLength, inputDescs.data(), &workSpaceSize);

        std::vector<T> workSpace(workSpaceSize / sizeof(T));
        std::vector<T> hiddenState(initHidden.size());

        auto input_dev = handle.Write(input);

        miopenGetRNNInputTensorSize(&handle, rnnDesc, seqLength, outputDescs.data(), &out_sz);
        std::vector<T> output(out_sz / sizeof(T));
        auto output_dev = handle.Write(output);

        auto weights_dev = handle.Write(weights);
        auto hy          = initHidden;
        std::fill(hy.begin(), hy.end(), 0.);
        auto cy = initCell;
        std::fill(cy.begin(), cy.end(), 0.);

        auto workSpace_dev = handle.Write(workSpace);

        std::vector<int> hlens(3, 0);
        hlens[0] = nLayers * (dirMode ? 2 : 1);
        hlens[1] = batch_seq[0];
        hlens[2] = hiddenSize;
        miopen::TensorDescriptor hiddenDesc(miopen::deref(rnnDesc).dataType, hlens.data(), 3);

        std::vector<int> wlen(1, 0);
        wlen[0] = weights.size();
        miopen::TensorDescriptor weightDesc(miopen::deref(rnnDesc).dataType, wlen.data(), 1);

        miopenRNNForwardInference(&handle,
                                  rnnDesc,
                                  seqLength,
                                  inputDescs.data(),
                                  input_dev.get(),
                                  &hiddenDesc,
                                  ((nohx) ? nullptr : handle.Write(initHidden).get()),
                                  &hiddenDesc,
                                  ((nocx) ? nullptr : handle.Write(initCell).get()),
                                  &weightDesc,
                                  weights_dev.get(),
                                  outputDescs.data(),
                                  output_dev.get(),
                                  &hiddenDesc,
                                  ((nohy) ? nullptr : handle.Write(hy).get()),
                                  &hiddenDesc,
                                  ((nocy) ? nullptr : handle.Write(cy).get()),
                                  workSpace_dev.get(),
                                  workSpaceSize);

#if(MIO_LSTM_TEST_DEBUG == 2)
        auto outdata = handle.Read<T>(output_dev, output.size());
        for(int i = 0; i < outdata.size(); i++)
        {
            printf("GPU outdata[%d]: %f\n", i, outdata[i]);
        }
#endif

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_end = std::chrono::high_resolution_clock::now();

        std::cout << "Wall clock: GPU forward_infer LSTM pass time: "
                  << std::chrono::duration<double>(t_end - t_start).count() << " seconds."
                  << std::endl;
#endif
#if(MIO_LSTM_TEST_DEBUG > 0)
        std::cout << "Done with LSTM forward inference GPU" << std::endl;
#endif
        return (handle.Read<T>(output_dev, output.size()));
    }

    void fail(int) const
    {
        std::cout << "./bin/MIOpenDriver rnn -n ";
        for(int i = 0; i < seqLength; i++)
        {
            if(i < seqLength - 1)
            {
                std::cout << batch_seq.at(i) << ",";
            }
            else
            {
                std::cout << batch_seq.at(i);
            }
        }
        std::cout << " -m lstm -k " << seqLength << " -H " << hiddenSize << " -W " << inputVecLen
                  << " -l " << nLayers << " -F 0 -r " << dirMode << " -b " << biasMode << " -p "
                  << inputMode << std::endl;

        std::cout << "inputMode: " << inputMode << " biasMode: " << biasMode
                  << " dirMode: " << dirMode << std::endl;
        std::cout << "hz: " << hiddenSize << " batch_n: " << batch_n << " seqLength: " << seqLength
                  << " inputLen: " << inputVecLen << " numLayers: " << nLayers << std::endl;
        std::cout << "Forward Inference LSTM: " << std::endl;
        std::cout << "Output tensor output failed verification." << std::endl;
    }
};
//~~~~~~~~~~~~ END FWD INFERENCE ~~~~~~~~~~~~~~~~~~~~~~~~

//****************************************************
// FORWARD TRAIN
//****************************************************
template <class T>
struct verify_forward_train_lstm
{
    std::vector<T> input;
    std::vector<T> initHidden;
    std::vector<T> initCell;
    std::vector<T> weights;
    std::vector<int> batch_seq;
    int hiddenSize;
    int seqLength;
    int nLayers;
    int biasMode;
    int dirMode;
    int inputMode;
    int batch_n;
    int inputVecLen;
    miopenRNNDescriptor_t rnnDesc;
    size_t realHiddenSize;
    bool nohx;
    bool nocx;
    bool nohy;
    bool nocy;

    verify_forward_train_lstm(miopenRNNDescriptor_t pRD,
                              const std::vector<T>& px,
                              const std::vector<T>& phx,
                              const std::vector<T>& pcx,
                              const std::vector<T>& pW,
                              const std::vector<int>& pBS,
                              const int pHS,
                              const int pBN,
                              const int pS,
                              const int pNL,
                              const int pBM,
                              const int pDM,
                              const int pIM,
                              const int pVL,
                              const size_t pHXZ,
                              const bool pnohx = false,
                              const bool pnocx = false,
                              const bool pnohy = false,
                              const bool pnocy = false)
    {
        rnnDesc     = pRD;
        input       = px;
        weights     = pW;
        batch_seq   = pBS;
        seqLength   = pS;
        nLayers     = pNL;
        biasMode    = pBM;
        dirMode     = pDM;
        inputMode   = pIM;
        batch_n     = pBN;
        hiddenSize  = pHS;
        inputVecLen = pVL;

        realHiddenSize = pHXZ;

        nohy = pnohy;
        nocy = pnocy;

        nohx = pnohx;
        if(!nohx)
            initHidden = phx; // this may be intentionally a nullptr
        else
            initHidden.resize(realHiddenSize);

        nocx = pnocx;
        if(!nocx)
            initCell = pcx; // this may
        else
            initCell.resize(realHiddenSize);
    }

    std::tuple<std::vector<T>, std::vector<T>, std::vector<T>, std::vector<T>> cpu()
    {

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_start = std::chrono::high_resolution_clock::now();
#endif

        auto&& handle = get_handle();

        int bi        = dirMode ? 2 : 1;
        int hy_h      = hiddenSize;
        int bi_stride = bi * hy_h;
        size_t out_sz = 0;
        size_t reserveSpaceSize;

        std::vector<miopen::TensorDescriptor> inputCPPDescs;
        std::vector<miopenTensorDescriptor_t> inputDescs;
        createTensorDescArray(
            inputCPPDescs, inputDescs, batch_seq, inputVecLen, miopen::deref(rnnDesc).dataType);

        std::vector<miopen::TensorDescriptor> outputCPPDescs;
        std::vector<miopenTensorDescriptor_t> outputDescs;
        createTensorDescArray(outputCPPDescs,
                              outputDescs,
                              batch_seq,
                              hiddenSize * ((dirMode) ? 2 : 1),
                              miopen::deref(rnnDesc).dataType);

        miopenGetRNNInputTensorSize(&handle, rnnDesc, seqLength, outputDescs.data(), &out_sz);
        miopenGetRNNTrainingReserveSize(
            &handle, rnnDesc, seqLength, inputDescs.data(), &reserveSpaceSize);
        std::vector<T> reserveSpace(reserveSpaceSize / sizeof(T));
        std::vector<T> output(out_sz / sizeof(T));
        std::vector<T> hiddenState(initHidden.size());
        std::vector<T> cellState(initCell.size());

        LSTMFwdCPUVerify(input,
                         weights,     // [ input_state_weight_trans
                                      // hidden_state_weight0_trans input1_trans
                                      // hidden1_trans ... output_weight;
                                      // bidirectional reversed weights ]
                         hiddenState, // current/final hidden state
                         initHidden,  // initial hidden state
                         cellState,   // current/final cell state
                         initCell,    // initial cell state
                         output,
                         batch_seq,       // input batch size
                         inputVecLen,     // input data length
                         seqLength,       // Number of iterations to unroll over
                         dirMode,         // whether using bidirectional net
                         biasMode,        // whether using bias
                         bi * nLayers,    // 1 by numlayer (number of stacks of hidden layers) for
                                          // unidirection, 2 by numlayer for bidirection
                         batch_seq.at(0), // equal to input batch size in_n[0]
                         hiddenSize,      // hidden state number
                         bi_stride,       // 1 by hy_h related function for unidirection, 2 by hy_h
                                          // related function for bidirection
                         inputMode,
                         reserveSpace,
                         nohx,
                         nocx);

#if(MIO_LSTM_TEST_DEBUG == 2)
        for(int i = 0; i < output.size(); i++)
        {
            std::cout << "CPU outdata[" << i << "]: " << output[i] << std::endl;
        }
#endif

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_end = std::chrono::high_resolution_clock::now();

        std::cout << "Wall clock: CPU forward train LSTM pass time: "
                  << std::chrono::duration<double>(t_end - t_start).count() << " seconds."
                  << std::endl;
#endif

        auto retSet = std::make_tuple(
            output, (nohy ? initHidden : hiddenState), (nocy ? initCell : cellState), reserveSpace);

#if(MIO_LSTM_TEST_DEBUG > 0)
        std::cout << "Done with LSTM forward train CPU" << std::endl;
        std::cout << "---------------------------------\n" << std::endl;
#endif
        return retSet;
    }

    std::tuple<std::vector<T>, std::vector<T>, std::vector<T>, std::vector<T>> gpu()
    {

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_start = std::chrono::high_resolution_clock::now();
#endif

        auto&& handle = get_handle();

        size_t out_sz           = 0;
        size_t workSpaceSize    = 0;
        size_t reserveSpaceSize = 0;

        std::vector<miopen::TensorDescriptor> inputCPPDescs;
        std::vector<miopenTensorDescriptor_t> inputDescs;
        createTensorDescArray(
            inputCPPDescs, inputDescs, batch_seq, inputVecLen, miopen::deref(rnnDesc).dataType);

        std::vector<miopen::TensorDescriptor> outputCPPDescs;
        std::vector<miopenTensorDescriptor_t> outputDescs;
        createTensorDescArray(outputCPPDescs,
                              outputDescs,
                              batch_seq,
                              hiddenSize * ((dirMode) ? 2 : 1),
                              miopen::deref(rnnDesc).dataType);

        miopenGetRNNWorkspaceSize(&handle, rnnDesc, seqLength, inputDescs.data(), &workSpaceSize);
        miopenGetRNNTrainingReserveSize(
            &handle, rnnDesc, seqLength, inputDescs.data(), &reserveSpaceSize);

        std::vector<T> workSpace(workSpaceSize / sizeof(T));
        std::vector<T> reserveSpace(reserveSpaceSize / sizeof(T));
        std::vector<T> hiddenState(initHidden.size());

        auto input_dev = handle.Write(input);

        miopenGetRNNInputTensorSize(&handle, rnnDesc, seqLength, outputDescs.data(), &out_sz);
        std::vector<T> output(out_sz / sizeof(T));
        auto output_dev = handle.Write(output);

        auto weights_dev = handle.Write(weights);
        auto hy          = initHidden;
        std::fill(hy.begin(), hy.end(), 0.);
        auto hy_dev = handle.Write(hy);
        auto cy     = initCell;
        std::fill(cy.begin(), cy.end(), 0.);
        auto cy_dev = handle.Write(cy);

        auto workSpace_dev    = handle.Write(workSpace);
        auto reserveSpace_dev = handle.Write(reserveSpace);

        std::vector<int> hlens(3, 0);
        hlens[0] = nLayers * (dirMode ? 2 : 1);
        hlens[1] = batch_seq[0];
        hlens[2] = hiddenSize;
        miopen::TensorDescriptor hiddenDesc(miopen::deref(rnnDesc).dataType, hlens.data(), 3);

        std::vector<int> wlen(1, 0);
        wlen[0] = weights.size();
        miopen::TensorDescriptor weightDesc(miopen::deref(rnnDesc).dataType, wlen.data(), 1);

        miopenRNNForwardTraining(&handle,
                                 rnnDesc,
                                 seqLength,
                                 inputDescs.data(),
                                 input_dev.get(),
                                 &hiddenDesc,
                                 ((nohx) ? nullptr : handle.Write(initHidden).get()),
                                 &hiddenDesc,
                                 ((nocx) ? nullptr : handle.Write(initCell).get()),
                                 &weightDesc,
                                 weights_dev.get(),
                                 outputDescs.data(),
                                 output_dev.get(),
                                 &hiddenDesc,
                                 ((nohy) ? nullptr : hy_dev.get()),
                                 &hiddenDesc,
                                 ((nocy) ? nullptr : cy_dev.get()),
                                 workSpace_dev.get(),
                                 workSpaceSize,
                                 reserveSpace_dev.get(),
                                 reserveSpaceSize);

#if(MIO_LSTM_TEST_DEBUG == 2)
        auto outdata = handle.Read<T>(output_dev, output.size());
        for(int i = 0; i < outdata.size(); i++)
        {
            printf("GPU outdata[%d]: %f\n", i, outdata[i]);
        }
#endif

        auto retSet =
            std::make_tuple(handle.Read<T>(output_dev, output.size()),
                            (nohy ? initHidden : handle.Read<T>(hy_dev, hy.size())),
                            (nocy ? initCell : handle.Read<T>(cy_dev, cy.size())),
                            handle.Read<T>(reserveSpace_dev, reserveSpaceSize / sizeof(T)));

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_end = std::chrono::high_resolution_clock::now();

        std::cout << "Wall clock: GPU forward_train LSTM pass time: "
                  << std::chrono::duration<double>(t_end - t_start).count() << " seconds."
                  << std::endl;
#endif
#if(MIO_LSTM_TEST_DEBUG > 0)
        std::cout << "Done with RNN forward train GPU" << std::endl;
#endif
        return retSet;
    }

    void fail(int badtensor) const
    {

        std::cout << "./bin/MIOpenDriver rnn -n ";
        for(int i = 0; i < seqLength; i++)
        {
            if(i < seqLength - 1)
            {
                std::cout << batch_seq.at(i) << ",";
            }
            else
            {
                std::cout << batch_seq.at(i);
            }
        }
        std::cout << " -m lstm "
                  << " -k " << seqLength << " -H " << hiddenSize << " -W " << inputVecLen << " -l "
                  << nLayers << " -F 0 "
                  << " -r " << dirMode << " -b " << biasMode << " -p " << inputMode << std::endl;

        std::cout << "inputMode: " << inputMode << " biasMode: " << biasMode
                  << " dirMode: " << dirMode << std::endl;
        std::cout << "hz: " << hiddenSize << " batch_n: " << batch_n << " seqLength: " << seqLength
                  << " inputLen: " << inputVecLen << " numLayers: " << nLayers << std::endl;
        std::cout << "Forward Train LSTM: " << std::endl;

        switch(badtensor)
        {
        case(0): std::cout << "Output tensor output failed verification." << std::endl; break;
        case(1): std::cout << "Hidden state tensor failed verification." << std::endl; break;
        case(2): std::cout << "Cell state tensor failed verification." << std::endl; break;
        case(3): std::cout << "Weight tensor failed verification." << std::endl; break;
        case(4): std::cout << "Reserved space tensor failed verification." << std::endl; break;
        default: break;
        }
    }
};
//~~~~~~~~~~~~ END FWD TRAIN ~~~~~~~~~~~~~~~~~~~~~~~~

//****************************************************
// BACKWARDS DATA
//****************************************************
template <class T>
struct verify_backward_data_lstm
{
    std::vector<T> yin;        // Y
    std::vector<T> dy;         // dY
    std::vector<T> dhy;        // dHY
    std::vector<T> dcy;        // dHY
    std::vector<T> initHidden; // HX
    std::vector<T> initCell;   // CX
    std::vector<T> weights;
    std::vector<T> reserveSpace;
    std::vector<int> batch_seq;
    int hiddenSize;
    int seqLength;
    int nLayers;
    int biasMode;
    int dirMode;
    int inputMode;
    int batch_n;
    int inputVecLen;
    miopenRNNDescriptor_t rnnDesc;
    size_t realHiddenSize;
    bool nohx;
    bool nocx;
    bool nodhy;
    bool nodcy;
    bool nodhx;
    bool nodcx;

    verify_backward_data_lstm(miopenRNNDescriptor_t pRD,
                              const std::vector<T>& py,
                              const std::vector<T>& pdy,
                              const std::vector<T>& pdhy,
                              const std::vector<T>& phx,
                              const std::vector<T>& pdcy,
                              const std::vector<T>& pcx,
                              const std::vector<T>& pW,
                              const std::vector<T>& pRS,
                              const std::vector<int>& pBS,
                              const int pHS,
                              const int pBN,
                              const int pS,
                              const int pNL,
                              const int pBM,
                              const int pDM,
                              const int pIM,
                              const int pVL,
                              const size_t pHXZ,
                              const bool pnohx  = false,
                              const bool pnocx  = false,
                              const bool pnodhy = false,
                              const bool pnodcy = false,
                              const bool pnodhx = false,
                              const bool pnodcx = false)
    {
        rnnDesc = pRD;
        yin     = py;
        dy      = pdy;
        weights = pW, reserveSpace = pRS;
        batch_seq      = pBS;
        seqLength      = pS;
        nLayers        = pNL;
        biasMode       = pBM;
        dirMode        = pDM;
        inputMode      = pIM;
        batch_n        = pBN;
        hiddenSize     = pHS;
        inputVecLen    = pVL;
        realHiddenSize = pHXZ;

        nodhx = pnodhx;
        nodcx = pnodcx;

        nohx = pnohx;
        if(!nohx)
            initHidden = phx; // this may be intentionally a nullptr
        else
            initHidden.resize(realHiddenSize);

        nocx = pnocx;
        if(!nocx)
            initCell = pcx; // this may be intentionally a nullptr
        else
            initCell.resize(realHiddenSize);

        nodhy = pnodhy;
        if(!nodhy)
            dhy = pdhy; // this may be intentionally a nullptr
        else
            dhy.resize(realHiddenSize);

        nodcy = pnodcy;
        if(!nodcy)
            dcy = pdcy; // this may be intentionally a nullptr
        else
            dcy.resize(realHiddenSize);
    }

    std::tuple<std::vector<T>, std::vector<T>, std::vector<T>, std::vector<T>, std::vector<T>> cpu()
    {

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_start = std::chrono::high_resolution_clock::now();
#endif

        auto&& handle = get_handle();

        int bi        = dirMode ? 2 : 1;
        int hy_h      = hiddenSize;
        int bi_stride = bi * hy_h;
        size_t workSpaceSize;

        std::vector<miopen::TensorDescriptor> inputCPPDescs;
        std::vector<miopenTensorDescriptor_t> inputDescs;
        createTensorDescArray(
            inputCPPDescs, inputDescs, batch_seq, inputVecLen, miopen::deref(rnnDesc).dataType);

        // Outputs ----------
        size_t in_sz = 0;
        miopenGetRNNInputTensorSize(&handle, rnnDesc, seqLength, inputDescs.data(), &in_sz);
        miopenGetRNNWorkspaceSize(&handle, rnnDesc, seqLength, inputDescs.data(), &workSpaceSize);
        std::vector<T> workSpace(workSpaceSize / sizeof(T));
        std::vector<T> dx(in_sz / sizeof(T));
        std::vector<T> dhx(initHidden.size());
        std::vector<T> dcx(initHidden.size());

        LSTMBwdDataCPUVerify(dx,              // DX (output)
                             weights,         // [ input_state_weight_trans
                                              //   hidden_state_weight0_trans input1_trans
                                              //   hidden1_trans ... output_weight;
                                              //   bidirectional reversed weights ]
                             dhy,             // current/final hidden state
                             dhx,             // DHX (output)
                             initHidden,      // HX initial hidden state
                             dcy,             // DCY current/final cell state
                             dcx,             // DCX (output)
                             initCell,        // CX
                             yin,             // Y
                             dy,              // DY
                             batch_seq,       // input batch size
                             inputVecLen,     // input data length
                             seqLength,       // Number of iterations to unroll over
                             dirMode,         // whether using bidirectional net
                             biasMode,        // whether using bias
                             bi * nLayers,    // 1 by numlayer (number of stacks of hidden layers)
                                              // for unidirection, 2 by numlayer for bidirection
                             batch_seq.at(0), // equal to input batch size in_n[0]
                             hiddenSize,      // hidden state number
                             bi_stride,       // 1 by hy_h related function for unidirection, 2 by
                             // hy_h related function for bidirection
                             inputMode,
                             reserveSpace,
                             workSpace,
                             nocx,
                             nodhy,
                             nodcy);

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_end = std::chrono::high_resolution_clock::now();

        std::cout << "Wall clock: CPU backward data LSTM pass time: "
                  << std::chrono::duration<double>(t_end - t_start).count() << " seconds."
                  << std::endl;
#endif

        auto retSet = std::make_tuple(
            dx, (nodhx ? initHidden : dhx), (nodcx ? initCell : dcx), reserveSpace, workSpace);

#if(MIO_LSTM_TEST_DEBUG > 0)
        std::cout << "Done with LSTM backward data CPU" << std::endl;
        std::cout << "---------------------------------\n" << std::endl;
#endif
        return retSet;
    }

    std::tuple<std::vector<T>, std::vector<T>, std::vector<T>, std::vector<T>, std::vector<T>> gpu()
    {

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_start = std::chrono::high_resolution_clock::now();
#endif

        auto&& handle = get_handle();

        size_t workSpaceSize = 0;

        std::vector<miopen::TensorDescriptor> inputCPPDescs;
        std::vector<miopenTensorDescriptor_t> inputDescs;
        createTensorDescArray(
            inputCPPDescs, inputDescs, batch_seq, inputVecLen, miopen::deref(rnnDesc).dataType);

        std::vector<miopen::TensorDescriptor> outputCPPDescs;
        std::vector<miopenTensorDescriptor_t> outputDescs;
        createTensorDescArray(outputCPPDescs,
                              outputDescs,
                              batch_seq,
                              hiddenSize * ((dirMode) ? 2 : 1),
                              miopen::deref(rnnDesc).dataType);

        miopenGetRNNWorkspaceSize(&handle, rnnDesc, seqLength, inputDescs.data(), &workSpaceSize);
        std::vector<T> workSpace(workSpaceSize / sizeof(T));
        auto workSpace_dev = handle.Write(workSpace);

        auto yin_dev          = handle.Write(yin);
        auto dyin_dev         = handle.Write(dy);
        auto reserveSpace_dev = handle.Write(reserveSpace);
        auto weights_dev      = handle.Write(weights);

        std::vector<int> hlens(3, 0);
        hlens[0] = nLayers * (dirMode ? 2 : 1);
        hlens[1] = batch_seq[0];
        hlens[2] = hiddenSize;
        miopen::TensorDescriptor hiddenDesc(miopen::deref(rnnDesc).dataType, hlens.data(), 3);

        std::vector<int> wlen(1, 0);
        wlen[0] = weights.size();
        miopen::TensorDescriptor weightDesc(miopen::deref(rnnDesc).dataType, wlen.data(), 1);

        size_t in_sz = 0;
        miopenGetRNNInputTensorSize(&handle, rnnDesc, seqLength, inputDescs.data(), &in_sz);
        std::vector<T> dx(in_sz / sizeof(T));
        auto dx_dev = handle.Write(dx);

        std::vector<T> dhx(initHidden.size());
        auto dhx_dev = handle.Write(dhx);

        std::vector<T> dcx(initHidden.size());
        auto dcx_dev = handle.Write(dcx);

        miopenRNNBackwardData(&handle,
                              rnnDesc,
                              seqLength,
                              outputDescs.data(),
                              yin_dev.get(),
                              outputDescs.data(),
                              dyin_dev.get(),
                              &hiddenDesc,
                              ((nodhy) ? nullptr : handle.Write(dhy).get()),
                              &hiddenDesc,
                              ((nodcy) ? nullptr : handle.Write(dcy).get()),
                              &weightDesc,
                              weights_dev.get(),
                              &hiddenDesc,
                              ((nohx) ? nullptr : handle.Write(initHidden).get()),
                              &hiddenDesc,
                              ((nocx) ? nullptr : handle.Write(initCell).get()),
                              inputDescs.data(),
                              dx_dev.get(),
                              &hiddenDesc,
                              ((nodhx) ? nullptr : dhx_dev.get()),
                              &hiddenDesc,
                              ((nodcx) ? nullptr : dcx_dev.get()),
                              workSpace_dev.get(),
                              workSpaceSize,
                              reserveSpace_dev.get(),
                              reserveSpace.size() * sizeof(T));

        auto retSet = std::make_tuple(handle.Read<T>(dx_dev, dx.size()),
                                      (nodhx ? initHidden : handle.Read<T>(dhx_dev, dhx.size())),
                                      (nodcx ? initCell : handle.Read<T>(dcx_dev, dcx.size())),
                                      handle.Read<T>(reserveSpace_dev, reserveSpace.size()),
                                      handle.Read<T>(workSpace_dev, workSpace.size()));

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_end = std::chrono::high_resolution_clock::now();

        std::cout << "Wall clock: GPU backward data LSTM pass time: "
                  << std::chrono::duration<double>(t_end - t_start).count() << " seconds."
                  << std::endl;
#endif
#if(MIO_LSTM_TEST_DEBUG > 0)
        std::cout << "Done with LSTM backward data GPU" << std::endl;
#endif
        return retSet;
    }

    void fail(int badtensor) const
    {

        std::cout << "./bin/MIOpenDriver rnn -n ";
        for(int i = 0; i < seqLength; i++)
        {
            if(i < seqLength - 1)
            {
                std::cout << batch_seq.at(i) << ",";
            }
            else
            {
                std::cout << batch_seq.at(i);
            }
        }
        std::cout << " -m lstm "
                  << " -k " << seqLength << " -H " << hiddenSize << " -W " << inputVecLen << " -l "
                  << nLayers << " -F 0 "
                  << " -r " << dirMode << " -b " << biasMode << " -p " << inputMode << std::endl;

        std::cout << "inputMode: " << inputMode << " biasMode: " << biasMode
                  << " dirMode: " << dirMode << std::endl;
        std::cout << "hz: " << hiddenSize << " batch_n: " << batch_n << " seqLength: " << seqLength
                  << " inputLen: " << inputVecLen << " numLayers: " << nLayers << std::endl;
        std::cout << "Backward Data LSTM: " << std::endl;

        switch(badtensor)
        {
        case(0): std::cout << "Output dx failed verification." << std::endl; break;
        case(1): std::cout << "Hidden state dhx tensor failed verification." << std::endl; break;
        case(2): std::cout << "Hidden cell dcx tensor failed verification." << std::endl; break;
        case(3): std::cout << "Reserved Space tensor failed verification." << std::endl; break;
        case(4): std::cout << "Workspace space tensor failed verification." << std::endl; break;
        default: break;
        }
    }
};
//~~~~~~~~~~~~ END BACKWARD DATA ~~~~~~~~~~~~~~~~~~~~~~~~

//****************************************************
// BACKWARDS WEIGHTS
//****************************************************
template <class T>
struct verify_backward_weights_lstm
{
    std::vector<T> input;      // Y
    std::vector<T> dy;         // dY
    std::vector<T> initHidden; // HX
    std::vector<T> reserveSpace;
    std::vector<T> workSpace;
    std::vector<int> batch_seq;
    int weightSize;
    int hiddenSize;
    int seqLength;
    int nLayers;
    int biasMode;
    int dirMode;
    int inputMode;
    int batch_n;
    int inputVecLen;
    miopenRNNDescriptor_t rnnDesc;
    size_t realHiddenSize;
    bool nohx;

    verify_backward_weights_lstm(miopenRNNDescriptor_t pRD,
                                 const std::vector<T>& px,
                                 const std::vector<T>& pdy,
                                 const std::vector<T>& phx,
                                 const std::vector<T>& pRS,
                                 const std::vector<T>& pWS,
                                 const std::vector<int>& pBS,
                                 const int pHS,
                                 const int pW,
                                 const int pBN,
                                 const int pS,
                                 const int pNL,
                                 const int pBM,
                                 const int pDM,
                                 const int pIM,
                                 const int pVL,
                                 const size_t pHXZ,
                                 const bool pnohx = false)
    {
        rnnDesc      = pRD;
        input        = px;
        dy           = pdy;
        reserveSpace = pRS;
        workSpace    = pWS;
        batch_seq    = pBS;
        seqLength    = pS;
        nLayers      = pNL;
        biasMode     = pBM;
        dirMode      = pDM;
        inputMode    = pIM;
        batch_n      = pBN;
        hiddenSize   = pHS;
        weightSize   = pW;
        inputVecLen  = pVL;

        realHiddenSize = pHXZ;

        nohx = pnohx;
        if(!nohx)
            initHidden = phx; // this may be intentionally a nullptr
        else
            initHidden.resize(realHiddenSize);
    }

    std::vector<T> cpu()
    {

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_start = std::chrono::high_resolution_clock::now();
#endif
        int bi        = dirMode ? 2 : 1;
        int hy_h      = hiddenSize;
        int bi_stride = bi * hy_h;
        std::vector<T> dweights(weightSize);

        LSTMBwdWeightCPUVerify(input,
                               dweights,   // (output) [ input_state_weight_trans
                                           // hidden_state_weight0_trans
                                           // input1_trans hidden1_trans ...
                                           // output_weight; bidirectional
                                           // reversed weights ]
                               initHidden, // initial hidden state
                               dy,
                               batch_seq,       // input batch size
                               inputVecLen,     // input data length
                               seqLength,       // Number of iterations to unroll over
                               dirMode,         // whether using bidirectional net
                               biasMode,        // whether using bias
                               bi * nLayers,    // 1 by numlayer (number of stacks of hidden
                                                // layers) for unidirection, 2 by numlayer for
                                                // bidirection
                               batch_seq.at(0), // equal to input batch size in_n[0]
                               hiddenSize,      // hidden state number
                               bi_stride,       // 1 by hy_h related function for unidirection, 2
                                                // by hy_h related function for bidirection
                               inputMode,
                               reserveSpace,
                               workSpace,
                               nohx);

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_end = std::chrono::high_resolution_clock::now();

        std::cout << "Wall clock: CPU backward_weights LSTM pass time: "
                  << std::chrono::duration<double>(t_end - t_start).count() << " seconds."
                  << std::endl;
#endif
#if(MIO_LSTM_TEST_DEBUG > 0)
        std::cout << "Done with LSTM backward weights CPU" << std::endl;
        std::cout << "---------------------------------\n" << std::endl;
#endif
        return dweights;
    }

    std::vector<T> gpu()
    {

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_start = std::chrono::high_resolution_clock::now();
#endif

        auto&& handle = get_handle();

        std::vector<miopen::TensorDescriptor> inputCPPDescs;
        std::vector<miopenTensorDescriptor_t> inputDescs;
        createTensorDescArray(
            inputCPPDescs, inputDescs, batch_seq, inputVecLen, miopen::deref(rnnDesc).dataType);

        std::vector<miopen::TensorDescriptor> outputCPPDescs;
        std::vector<miopenTensorDescriptor_t> outputDescs;
        createTensorDescArray(outputCPPDescs,
                              outputDescs,
                              batch_seq,
                              hiddenSize * ((dirMode) ? 2 : 1),
                              miopen::deref(rnnDesc).dataType);

        auto workSpace_dev    = handle.Write(workSpace);
        auto reserveSpace_dev = handle.Write(reserveSpace);
        std::vector<T> dweights(weightSize);
        auto dweights_dev = handle.Write(dweights);
        miopen::TensorDescriptor weightDesc(miopen::deref(rnnDesc).dataType, &weightSize, 1);

        std::vector<int> hlens(3, 0);
        hlens[0] = nLayers * (dirMode ? 2 : 1);
        hlens[1] = batch_seq[0];
        hlens[2] = hiddenSize;
        miopen::TensorDescriptor hiddenDesc(miopen::deref(rnnDesc).dataType, hlens.data(), 3);
        auto dy_dev    = handle.Write(dy);
        auto input_dev = handle.Write(input);

        miopenRNNBackwardWeights(&handle,
                                 rnnDesc,
                                 seqLength,
                                 inputDescs.data(),
                                 input_dev.get(),
                                 &hiddenDesc,
                                 ((nohx) ? nullptr : handle.Write(initHidden).get()),
                                 outputDescs.data(),
                                 dy_dev.get(),
                                 &weightDesc,
                                 dweights_dev.get(),
                                 workSpace_dev.get(),
                                 workSpace.size() * sizeof(T),
                                 reserveSpace_dev.get(),
                                 reserveSpace.size() * sizeof(T));

#if(MIO_RNN_TIME_EVERYTHING == 1)
        auto t_end = std::chrono::high_resolution_clock::now();

        std::cout << "Wall clock: GPU backwards_weights LSTM pass time: "
                  << std::chrono::duration<double>(t_end - t_start).count() << " seconds."
                  << std::endl;
#endif
#if(MIO_LSTM_TEST_DEBUG > 0)
        std::cout << "Done with LSTM backward weights GPU" << std::endl;
#endif
        auto retvec = handle.Read<T>(dweights_dev, dweights.size());
        return retvec;
    }

    void fail(int) const
    {
        std::cout << "./bin/MIOpenDriver rnn -n ";
        for(int i = 0; i < seqLength; i++)
        {
            if(i < seqLength - 1)
            {
                std::cout << batch_seq.at(i) << ",";
            }
            else
            {
                std::cout << batch_seq.at(i);
            }
        }
        std::cout << " -m lstm "
                  << " -k " << seqLength << " -H " << hiddenSize << " -W " << inputVecLen << " -l "
                  << nLayers << " -F 0 "
                  << " -r " << dirMode << " -b " << biasMode << " -p " << inputMode << std::endl;

        std::cout << "inputMode: " << inputMode << " biasMode: " << biasMode
                  << " dirMode: " << dirMode << std::endl;
        std::cout << "hz: " << hiddenSize << " batch_n: " << batch_n << " seqLength: " << seqLength
                  << " inputLen: " << inputVecLen << " numLayers: " << nLayers << std::endl;
        std::cout << "Backward Weights LSTM: " << std::endl;
    }
};
//~~~~~~~~~~~~ END BACKWARD WEIGHTS ~~~~~~~~~~~~~~~~~~~~~~~~

//====================== DRIVER ============================
template <class T>
struct lstm_driver : test_driver
{
    std::vector<int> batchSeq;
    int seqLength{};
    int inVecLen{};
    int hiddenSize{};
    int numLayers{};
    int inputMode{};
    int biasMode{};
    int dirMode{};
    int batchSize{};

    // Null pointer input
    bool nohx          = false;
    bool nodhy         = false;
    bool nocx          = false;
    bool nodcy         = false;
    bool nohy          = false;
    bool nodhx         = false;
    bool nocy          = false;
    bool nodcx         = false;
    bool flatBatchFill = false;

    lstm_driver()
    {
        std::vector<int> modes(2, 0);
        modes[1] = 1;
        std::vector<int> defaultBS(1);

        add(batchSize, "batch-size", generate_data(get_lstm_batchSize(), {17}));
        add(seqLength, "seq-len", generate_data(get_lstm_seq_len(), {2}));
        add(inVecLen, "vector-len", generate_data(get_lstm_vector_len()));
        add(hiddenSize, "hidden-size", generate_data(get_lstm_hidden_size()));
        add(numLayers, "num-layers", generate_data(get_lstm_num_layers()));
        add(nohx, "no-hx", flag());
        add(nodhy, "no-dhy", flag());
        add(nocx, "no-cx", flag());
        add(nodcy, "no-dcy", flag());
        add(nohy, "no-hy", flag());
        add(nodhx, "no-dhx", flag());
        add(nocy, "no-cy", flag());
        add(nodcx, "no-dcx", flag());
        add(flatBatchFill, "flat-batch-fill", flag());

#if(MIO_LSTM_TEST_DEBUG == 3)
        biasMode  = 0;
        dirMode   = 0;
        inputMode = 0;
#else
        add(inputMode, "in-mode", generate_data(modes));
        add(biasMode, "bias-mode", generate_data(modes));
        add(dirMode, "dir-mode", generate_data(modes));
#endif
        add(batchSeq,
            "batch-seq",
            lazy_generate_data([=] { return generate_batchSeq(batchSize, seqLength); }, defaultBS));
    }

    void run()
    {

#if(MIOPEN_BACKEND_OPENCL == 1)
        if(type == miopenHalf)
            exit(EXIT_SUCCESS);
#endif

        if(batchSeq.empty() || !batchSeq[0])
        {
            std::cout << "Empty batch sequence. Filling uniformly with batch size: " << batchSize
                      << std::endl;
            if(flatBatchFill)
            {
                batchSeq.clear();
                batchSeq.resize(seqLength, batchSize);
            }
            else
            {
                batchSeq = generate_batchSeq(batchSize, seqLength)[0];
            }
        }

        if(batchSeq.size() != seqLength)
        {
            std::cerr << "FAILED: Batch sequence vector length, does not match sequence length."
                      << std::endl;
            std::abort();
        }

#if(MIO_LSTM_TEST_DEBUG == 2)
        for(int i = 0; i < seqLength; i++)
        {
            std::cout << "batch seq[" << i << "]: " << batchSeq.at(i) << std::endl;
        }
#endif

        auto&& handle = get_handle();

        int batch_n = std::accumulate(batchSeq.begin(), batchSeq.end(), 0);

        miopenRNNDescriptor_t rnnDesc;
        miopenCreateRNNDescriptor(&rnnDesc);
        miopenRNNAlgo_t algoMode = miopenRNNdefault;
        miopenSetRNNDescriptor(rnnDesc,
                               hiddenSize,
                               numLayers,
                               miopenRNNInputMode_t(inputMode),
                               miopenRNNDirectionMode_t(dirMode),
                               miopenLSTM,
                               miopenRNNBiasMode_t(biasMode),
                               miopenRNNAlgo_t(algoMode),
                               type); // defined in superclass testdriver

        // Create input tensor
        // If we are in skip mode, take the real input size to be the vector length.
        auto inVecReal    = (inputMode) ? hiddenSize : inVecLen;
        std::size_t in_sz = inVecReal * batch_n;
        std::vector<T> input(in_sz);
        srand(0);
        for(std::size_t i = 0; i < in_sz; i++)
        {
            input[i] = /*(((rand()%2)==1)?-1:1)**/ 0.001 * float(rand() % 100);
        }

        std::size_t hx_sz = ((dirMode) ? 2 : 1) * hiddenSize * batchSize * numLayers;
        std::vector<T> hx(hx_sz);
        std::vector<T> cx(hx_sz);
        std::vector<T> dhyin(hx_sz);
        std::vector<T> dcyin(hx_sz);

        size_t wei_bytes = 0;
        std::vector<int> inlens(2, 0);
        inlens.at(0) = batchSeq.at(0);
        inlens.at(1) = inVecReal;
        auto firstInputDesc =
            miopen::TensorDescriptor(miopen::deref(rnnDesc).dataType, inlens.data(), 2);
        miopenGetRNNParamsSize(
            &handle, rnnDesc, &firstInputDesc, &wei_bytes, miopen::deref(rnnDesc).dataType);
        auto wei_sz = int(wei_bytes / sizeof(T));
        std::vector<T> weights(wei_sz);
        for(std::size_t i = 0; i < wei_sz; i++)
        {
            weights[i] = (((rand() % 2) == 1) ? -1 : 1) * 0.001 * float(rand() % 100);
        }

#if(MIO_LSTM_TEST_DEBUG > 0)
        printf("inputMode: %d, biasMode: %d, dirMode: %d\n", inputMode, biasMode, dirMode);
        printf("hz: %d, batch_n: %d, seqLength: %d, inputLen: %d, numLayers: %d\n",
               hiddenSize,
               batch_n,
               seqLength,
               inVecLen,
               numLayers);
        std::cout << "nohx: " << nohx;
        std::cout << ", nocx: " << nocx;
        std::cout << ", nodhy: " << nodhy;
        std::cout << ", nodcy: " << nodcy << std::endl;
        std::cout << "nohy: " << nohy;
        std::cout << ", nocy: " << nocy;
        std::cout << ", nodhx: " << nodhx;
        std::cout << ", nodcx: " << nodcx << std::endl;
#endif

        if(!nohx)
        {
            for(std::size_t i = 0; i < hx_sz; i++)
            {
                hx[i] = 0.001 * float(rand() % 100);
            }
        }

        if(!nodhy)
        {
            for(std::size_t i = 0; i < hx_sz; i++)
            {
                dhyin[i] = 0.001 * float(rand() % 100);
            }
        }

        if(!nocx)
        {
            for(std::size_t i = 0; i < hx_sz; i++)
            {
                cx[i] = 0.001 * float(rand() % 100);
            }
        }

        if(!nodcy)
        {
            for(std::size_t i = 0; i < hx_sz; i++)
            {
                dcyin[i] = 0.001 * float(rand() % 100);
            }
        }
        auto fwdTrainOutputPair = verify(verify_forward_train_lstm<T>{rnnDesc,
                                                                      input,
                                                                      hx,
                                                                      cx,
                                                                      weights,
                                                                      batchSeq,
                                                                      hiddenSize,
                                                                      batch_n,
                                                                      seqLength,
                                                                      numLayers,
                                                                      biasMode,
                                                                      dirMode,
                                                                      inputMode,
                                                                      inVecReal,
                                                                      hx_sz,
                                                                      nohx,
                                                                      nocx,
                                                                      nohy,
                                                                      nocy});

        /// RETURNS std::make_tuple(output, hiddenState, cellState, reserveSpace);
        auto yin                  = std::get<0>(fwdTrainOutputPair.second);
        auto curHiddenState       = std::get<1>(fwdTrainOutputPair.second);
        auto curCellState         = std::get<2>(fwdTrainOutputPair.second);
        auto reserveSpaceFwdTrain = std::get<3>(fwdTrainOutputPair.second);

        std::vector<T> dyin(yin.size());
        for(std::size_t i = 0; i < yin.size(); i++)
        {
            dyin[i] = /*(((rand()%2)==1)?-1:1)**/ 0.001 * float(rand() % 100);
        }

#if(MIO_LSTM_TEST_DEBUG > 0)
        printf("Running backward data LSTM.\n");
#endif
        auto bwdDataOutputPair =
            verify(verify_backward_data_lstm<T>{rnnDesc,   yin,        dyin,
                                                dhyin,     hx,         dcyin,
                                                cx,        weights,    reserveSpaceFwdTrain,
                                                batchSeq,  hiddenSize, batch_n,
                                                seqLength, numLayers,  biasMode,
                                                dirMode,   inputMode,  inVecReal,
                                                hx_sz,     nohx,       nocx,
                                                nodhy,     nodcy,      nodhx,
                                                nodcx});

        // RETURNS:  std::make_tuple(dx, dhx, dcx, reserveSpace, workSpace);
        auto reserveSpaceBwdData = std::get<3>(bwdDataOutputPair.second);
        auto workSpaceBwdData    = std::get<4>(bwdDataOutputPair.second);

#if(MIO_LSTM_TEST_DEBUG > 0)
        printf("Running backward weights LSTM.\n");
        printf("reserve sz: %d, workSpace sz: %d, weight sz: %d\n",
               reserveSpaceBwdData.size(),
               workSpaceBwdData.size(),
               wei_sz);
        fflush(nullptr);
#endif
        auto dweights_pair = verify(verify_backward_weights_lstm<T>{rnnDesc,
                                                                    input,
                                                                    dyin,
                                                                    hx,
                                                                    reserveSpaceBwdData,
                                                                    workSpaceBwdData,
                                                                    batchSeq,
                                                                    hiddenSize,
                                                                    wei_sz,
                                                                    batch_n,
                                                                    seqLength,
                                                                    numLayers,
                                                                    biasMode,
                                                                    dirMode,
                                                                    inputMode,
                                                                    inVecReal,
                                                                    hx_sz,
                                                                    nohx});

        verify(verify_forward_infer_lstm<T>{rnnDesc,
                                            input,
                                            hx,
                                            cx,
                                            weights,
                                            batchSeq,
                                            hiddenSize,
                                            batch_n,
                                            seqLength,
                                            numLayers,
                                            biasMode,
                                            dirMode,
                                            inputMode,
                                            inVecReal,
                                            hx_sz,
                                            nohx,
                                            nocx,
                                            nohy,
                                            nocy});

        /* normal hx/cx/dhy/dcy input test end */

        // DLOWELL: Subtracting delta weights may produce NAN and infinities. Further investigation
        // is needed.
        //        auto dweights = std::get<1>(dweights_pair);
        //        std::transform(weightData.begin( ), weightData.end( ), dweights.begin( ),
        //        weightData.begin( ),std::minus<T>( ));
        //        verify(verify_forward_infer_lstm<T>{rnnDesc, inputData,
        //                                        curHiddenState, curCellState, weightData,
        //                                        batchSeq,
        //                                        hiddenSize, batch_n,
        //                                        seqLength, numLayers,
        //                                        biasMode, dirMode,
        //                                        inputMode, inVecReal});
    }
};

int main(int argc, const char* argv[])
{
#if(MIO_RNN_TIME_EVERYTHING > 0)
    auto t_start = std::chrono::high_resolution_clock::now();
#endif
    test_drive<lstm_driver>(argc, argv);

#if(MIO_RNN_TIME_EVERYTHING > 0)
    auto t_end = std::chrono::high_resolution_clock::now();

    std::cout << "Wall clock: RNN test pass time: "
              << std::chrono::duration<double>(t_end - t_start).count() << " seconds." << std::endl;
#endif
    exit(0);
}
