//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

__kernel void mask_evaluate_r8(__write_only image2d_t dst) {
  const int2 gid  = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int2 size = get_image_dim(dst);
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  write_imagef(dst, gid, (float4)(1.0f, 0.0f, 0.0f, 1.0f));
}
