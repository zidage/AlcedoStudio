//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/operators/cpu/debayer_amaze.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

namespace alcedo {
namespace CPU {

// Constants for AMaZe
static constexpr float GRAD_THRESH     = 0.0825f;
static constexpr float ARTIFACT_THRESH = 0.1f;
static constexpr int   ITERATIONS      = 0;

// RGGB Bayer Pattern Mapping
static int             fc[2][2]        = {{0, 1}, {3, 2}};  // R=0, G1=1, B=2, G2=3
#define FC(y, x)       fc[(y) & 1][(x) & 1]
#define IS_GREEN(y, x) (FC(y, x) == 1 || FC(y, x) == 3)

// Utility functions
inline float SQR(float x) { return x * x; }
inline float MIN3(float a, float b, float c) { return std::min(a, std::min(b, c)); }
inline float MAX3(float a, float b, float c) { return std::max(a, std::max(b, c)); }
inline float LIM(float x, float min, float max) { return std::max(min, std::min(x, max)); }

class AMaZeDemosaic {
 private:
  int       height_, width_;
  cv::Mat1f raw_;
  cv::Mat1f red_, green_, blue_;

  // Working buffers
  cv::Mat1f hvgrad_;    // Horizontal/Vertical gradients
  cv::Mat1f dggrad_;    // Diagonal gradients
  cv::Mat1f nyquist_;   // Nyquist frequency map
  cv::Mat1f artifact_;  // Artifact map

  // Temporary interpolation buffers
  cv::Mat1f green_h_, green_v_;  // Horizontal and vertical green estimates
  cv::Mat1f diff_h_, diff_v_;    // Color difference maps

 public:
  AMaZeDemosaic(const cv::Mat1f& bayer) : raw_(bayer) {
    height_   = raw_.rows;
    width_    = raw_.cols;

    // Initialize output channels
    red_      = cv::Mat1f(height_, width_, 0.0f);
    green_    = cv::Mat1f(height_, width_, 0.0f);
    blue_     = cv::Mat1f(height_, width_, 0.0f);

    // Initialize working buffers
    hvgrad_   = cv::Mat1f(height_, width_, 0.0f);
    dggrad_   = cv::Mat1f(height_, width_, 0.0f);
    nyquist_  = cv::Mat1f(height_, width_, 0.0f);
    artifact_ = cv::Mat1f(height_, width_, 0.0f);
    // green_h  = cv::Mat1f(height, width, 0.0f);
    // green_v  = cv::Mat1f(height, width, 0.0f);
    diff_h_   = cv::Mat1f(height_, width_, 0.0f);
    diff_v_   = cv::Mat1f(height_, width_, 0.0f);

    // Copy raw values to appropriate channels
    initializeChannels();
  }

  void initializeChannels() {
#pragma omp parallel for
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        float val = raw_(y, x);
        switch (FC(y, x)) {
          case 0:
            red_(y, x) = val;
            break;  // R
          case 1:
            green_(y, x) = val;
            break;  // G1
          case 2:
            blue_(y, x) = val;
            break;  // B
          case 3:
            green_(y, x) = val;
            break;  // G2
        }
      }
    }
  }

  void computeGradients() {
#pragma omp parallel for
    for (int y = 2; y < height_ - 2; ++y) {
      for (int x = 2; x < width_ - 2; ++x) {
        if (!IS_GREEN(y, x)) {
          // Horizontal gradient
          float grad_h = std::abs(raw_(y, x - 1) - raw_(y, x + 1));
          if (x >= 3 && x < width_ - 3) {
            grad_h += std::abs(raw_(y, x - 2) - raw_(y, x)) * 0.5f;
            grad_h += std::abs(raw_(y, x + 2) - raw_(y, x)) * 0.5f;
            // Add second derivative
            grad_h += std::abs(2 * raw_(y, x) - raw_(y, x - 2) - raw_(y, x + 2)) * 0.25f;
          }

          // Vertical gradient
          float grad_v = std::abs(raw_(y - 1, x) - raw_(y + 1, x));
          if (y >= 3 && y < height_ - 3) {
            grad_v += std::abs(raw_(y - 2, x) - raw_(y, x)) * 0.5f;
            grad_v += std::abs(raw_(y + 2, x) - raw_(y, x)) * 0.5f;
            // Add second derivative
            grad_v += std::abs(2 * raw_(y, x) - raw_(y - 2, x) - raw_(y + 2, x)) * 0.25f;
          }

          hvgrad_(y, x)  = grad_h - grad_v;

          // Diagonal gradients
          float grad_ne = std::abs(raw_(y - 1, x + 1) - raw_(y + 1, x - 1));
          float grad_nw = std::abs(raw_(y - 1, x - 1) - raw_(y + 1, x + 1));
          dggrad_(y, x)  = grad_ne - grad_nw;
        }
      }
    }
  }

  void interpolateGreenInitial() {
#pragma omp parallel for
    for (int y = 2; y < height_ - 2; ++y) {
      for (int x = 2; x < width_ - 2; ++x) {
        if (!IS_GREEN(y, x)) {
          float grad = hvgrad_(y, x);

          // Horizontal interpolation
          float g_h  = (raw_(y, x - 1) + raw_(y, x + 1)) * 0.5f;
          if (x >= 2 && x < width_ - 2) {
            // Add Laplacian correction
            g_h += (2 * raw_(y, x) - raw_(y, x - 2) - raw_(y, x + 2)) * 0.25f;
          }
          // green_h(y, x) = g_h;

          // Vertical interpolation
          float g_v = (raw_(y - 1, x) + raw_(y + 1, x)) * 0.5f;
          if (y >= 2 && y < height_ - 2) {
            // Add Laplacian correction
            g_v += (2 * raw_(y, x) - raw_(y - 2, x) - raw_(y + 2, x)) * 0.25f;
          }
          // green_v(y, x) = g_v;
          // Adaptive combination based on gradients
          if (std::abs(grad) < GRAD_THRESH) {
            // Similar gradients - use weighted average
            green_(y, x) = (g_h + g_v) * 0.5f;
          } else if (grad > 0) {
            // Vertical edge - use horizontal interpolation
            green_(y, x) = g_h;
          } else {
            // Horizontal edge - use vertical interpolation
            green_(y, x) = g_v;
          }
        }
      }
    }
  }

  void computeNyquistMap() {
#pragma omp parallel for
    for (int y = 4; y < height_ - 4; ++y) {
      for (int x = 4; x < width_ - 4; ++x) {
        if (!IS_GREEN(y, x)) {
          // Compute local Nyquist frequency content
          float nyq = 0;

          // Check for high-frequency patterns in different directions
          for (int dy = -2; dy <= 2; dy += 2) {
            for (int dx = -2; dx <= 2; dx += 2) {
              if (dy == 0 && dx == 0) continue;

              float diff      = std::abs(green_(y + dy, x + dx) - green_(y, x));
              float local_avg = (green_(y + dy, x + dx) + green_(y, x)) * 0.5f;
              if (local_avg > 0) {
                nyq += diff / (local_avg + 1e-6f);
              }
            }
          }

          nyquist_(y, x) = nyq / 8.0f;  // Normalize
        }
      }
    }
  }

  void detectAndReduceArtifacts() {
// Detect artifacts (zipper, maze patterns)
#pragma omp parallel for
    for (int y = 3; y < height_ - 3; ++y) {
      for (int x = 3; x < width_ - 3; ++x) {
        if (!IS_GREEN(y, x)) {
          float art = 0;

          // Check for alternating patterns (zipper artifacts)
          if (x >= 4 && x < width_ - 4) {
            float alt_h = std::abs(green_(y, x - 2) - 2 * green_(y, x) + green_(y, x + 2));
            art += alt_h;
          }
          if (y >= 4 && y < height_ - 4) {
            float alt_v = std::abs(green_(y - 2, x) - 2 * green_(y, x) + green_(y + 2, x));
            art += alt_v;
          }

          // Combine with Nyquist map
          artifact_(y, x) = art * nyquist_(y, x);
        }
      }
    }

// Reduce artifacts where detected
#pragma omp parallel for
    for (int y = 3; y < height_ - 3; ++y) {
      for (int x = 3; x < width_ - 3; ++x) {
        if (!IS_GREEN(y, x) && artifact_(y, x) > ARTIFACT_THRESH) {
          // Use median of surrounding green values
          std::array<float, 8> neighbors;
          int                  idx = 0;
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              if (dy == 0 && dx == 0) continue;
              neighbors[idx++] = green_(y + dy, x + dx);
            }
          }
          std::nth_element(neighbors.begin(), neighbors.begin() + 4, neighbors.begin() + 8);

          // Blend with median based on artifact strength
          float blend = std::min(1.0f, artifact_(y, x) / 0.5f);
          green_(y, x) = green_(y, x) * (1 - blend) + neighbors[4] * blend;
        }
      }
    }
  }

  void refineGreenChannel() {
    cv::Mat1f green_new = green_.clone();

#pragma omp parallel for
    for (int y = 3; y < height_ - 3; ++y) {
      for (int x = 3; x < width_ - 3; ++x) {
        if (!IS_GREEN(y, x)) {
          // Compute color differences in neighborhood
          float sum_weight = 0;
          float sum_green  = 0;

          for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
              if (std::abs(dy) + std::abs(dx) > 3) continue;

              int ny = y + dy;
              int nx = x + dx;

              if (ny >= 0 && ny < height_ && nx >= 0 && nx < width_) {
                // Weight based on color similarity and distance
                float color_diff   = std::abs(raw_(ny, nx) - raw_(y, x));
                float spatial_dist = std::sqrt(float(dy * dy + dx * dx));
                float weight       = std::exp(-color_diff * 0.1f - spatial_dist * 0.5f);

                sum_green += green_(ny, nx) * weight;
                sum_weight += weight;
              }
            }
          }

          if (sum_weight > 0) {
            green_new(y, x) = sum_green / sum_weight;
          }
        }
      }
    }

    green_ = green_new;
  }

  void interpolateRedBlue() {
#pragma omp parallel for
    for (int y = 2; y < height_ - 2; ++y) {
      for (int x = 2; x < width_ - 2; ++x) {
        int color = FC(y, x);

        if (IS_GREEN(y, x)) {
          // At green pixels, interpolate both R and B
          float sum_r = 0, sum_b = 0;
          float weight_r = 0, weight_b = 0;

          // Use color difference interpolation
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              if (std::abs(dy) + std::abs(dx) != 1) continue;

              int   ny     = y + dy;
              int   nx     = x + dx;
              int   nc     = FC(ny, nx);

              float g_diff = std::abs(green_(ny, nx) - green_(y, x));
              float weight = 1.0f / (1.0f + g_diff);

              if (nc == 0) {  // Red pixel
                sum_r += (red_(ny, nx) - green_(ny, nx)) * weight;
                weight_r += weight;
              } else if (nc == 2) {  // Blue pixel
                sum_b += (blue_(ny, nx) - green_(ny, nx)) * weight;
                weight_b += weight;
              }
            }
          }

          if (weight_r > 0.f) red_(y, x) = green_(y, x) + sum_r / weight_r;
          if (weight_b > 0.f) blue_(y, x) = green_(y, x) + sum_b / weight_b;

        } else if (color == 0) {
          // At red pixels, interpolate blue
          float sum_b    = 0;
          float weight_b = 0;

          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              if (std::abs(dy) != 1 || std::abs(dx) != 1) continue;

              int   ny     = y + dy;
              int   nx     = x + dx;

              float g_diff = std::abs(green_(ny, nx) - green_(y, x));
              float weight = 1.0f / (1.0f + g_diff);

              sum_b += (blue_(ny, nx) - green_(ny, nx)) * weight;
              weight_b += weight;
            }
          }

          if (weight_b > 0) blue_(y, x) = green_(y, x) + sum_b / weight_b;

        } else if (color == 2) {
          // At blue pixels, interpolate red
          float sum_r    = 0;
          float weight_r = 0;

          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              if (std::abs(dy) != 1 || std::abs(dx) != 1) continue;

              int   ny     = y + dy;
              int   nx     = x + dx;

              float g_diff = std::abs(green_(ny, nx) - green_(y, x));
              float weight = 1.0f / (1.0f + g_diff);

              sum_r += (red_(ny, nx) - green_(ny, nx)) * weight;
              weight_r += weight;
            }
          }

          if (weight_r > 0) red_(y, x) = green_(y, x) + sum_r / weight_r;
        }
      }
    }
  }

  void suppressFalseColors() {
    cv::Mat1f red_new  = red_.clone();
    cv::Mat1f blue_new = blue_.clone();

#pragma omp parallel for
    for (int y = 2; y < height_ - 2; ++y) {
      for (int x = 2; x < width_ - 2; ++x) {
        // Compute median of color differences in 3x3 neighborhood
        std::array<float, 9> cd_r, cd_b;
        int                  idx = 0;

        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            cd_r[idx] = red_(y + dy, x + dx) - green_(y + dy, x + dx);
            cd_b[idx] = blue_(y + dy, x + dx) - green_(y + dy, x + dx);
            idx++;
          }
        }

        std::nth_element(cd_r.begin(), cd_r.begin() + 4, cd_r.end());
        std::nth_element(cd_b.begin(), cd_b.begin() + 4, cd_b.end());

        float median_cd_r  = cd_r[4];
        float median_cd_b  = cd_b[4];

        float current_cd_r = red_(y, x) - green_(y, x);
        float current_cd_b = blue_(y, x) - green_(y, x);

        // Suppress if too different from median
        float thresh_r     = std::max(0.1f, std::abs(median_cd_r) * 2.0f);
        float thresh_b     = std::max(0.1f, std::abs(median_cd_b) * 2.0f);

        if (std::abs(current_cd_r - median_cd_r) > thresh_r) {
          red_new(y, x) = green_(y, x) + median_cd_r;
        }
        if (std::abs(current_cd_b - median_cd_b) > thresh_b) {
          blue_new(y, x) = green_(y, x) + median_cd_b;
        }
      }
    }

    red_  = red_new;
    blue_ = blue_new;
  }

  void process() {
    // Step 1: Compute initial gradients
    computeGradients();

    // Step 2: Initial green interpolation
    interpolateGreenInitial();

    // Step 3: Iterative refinement
    for (int iter = 0; iter < ITERATIONS; ++iter) {
      // Compute Nyquist frequency map
      computeNyquistMap();

      // Detect and reduce artifacts
      detectAndReduceArtifacts();

      // Refine green channel
      refineGreenChannel();
    }

    // Step 4: Interpolate red and blue channels
    interpolateRedBlue();

    // Step 5: False color suppression
    suppressFalseColors();

    // Handle image boundaries (simple bilinear for now)
    handleBoundaries();
  }

  void handleBoundaries() {
    // Simple boundary handling - can be improved
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        if (y < 2 || y >= height_ - 2 || x < 2 || x >= width_ - 2) {
          // Use nearest valid pixel
          int sy      = LIM(y, 2, height_ - 3);
          int sx      = LIM(x, 2, width_ - 3);

          red_(y, x)   = red_(sy, sx);
          green_(y, x) = green_(sy, sx);
          blue_(y, x)  = blue_(sy, sx);
        }
      }
    }
  }

  void getResult(cv::Mat& output) {
    std::vector<cv::Mat> channels = {red_, green_, blue_};
    cv::merge(channels, output);
  }
};

void BayerRGGB2RGB_AMaZe(cv::Mat& bayer_io) {
  // Convert to float if needed
  cv::Mat1f bayer_float;
  if (bayer_io.type() != CV_32F) {
    bayer_io.convertTo(bayer_float, CV_32F);
  } else {
    bayer_float = bayer_io;
  }

  // Process with AMaZe
  AMaZeDemosaic amaze(bayer_float);
  amaze.process();

  // Get result
  amaze.getResult(bayer_io);
}

}  // namespace CPU
}  // namespace alcedo