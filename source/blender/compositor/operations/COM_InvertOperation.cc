/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2011, Blender Foundation.
 */

#include "COM_InvertOperation.h"

namespace blender::compositor {

InvertOperation::InvertOperation()
{
  this->add_input_socket(DataType::Value);
  this->add_input_socket(DataType::Color);
  this->add_output_socket(DataType::Color);
  input_value_program_ = nullptr;
  input_color_program_ = nullptr;
  color_ = true;
  alpha_ = false;
  set_canvas_input_index(1);
  this->flags.can_be_constant = true;
}
void InvertOperation::init_execution()
{
  input_value_program_ = this->get_input_socket_reader(0);
  input_color_program_ = this->get_input_socket_reader(1);
}

void InvertOperation::execute_pixel_sampled(float output[4],
                                            float x,
                                            float y,
                                            PixelSampler sampler)
{
  float input_value[4];
  float input_color[4];
  input_value_program_->read_sampled(input_value, x, y, sampler);
  input_color_program_->read_sampled(input_color, x, y, sampler);

  const float value = input_value[0];
  const float inverted_value = 1.0f - value;

  if (color_) {
    output[0] = (1.0f - input_color[0]) * value + input_color[0] * inverted_value;
    output[1] = (1.0f - input_color[1]) * value + input_color[1] * inverted_value;
    output[2] = (1.0f - input_color[2]) * value + input_color[2] * inverted_value;
  }
  else {
    copy_v3_v3(output, input_color);
  }

  if (alpha_) {
    output[3] = (1.0f - input_color[3]) * value + input_color[3] * inverted_value;
  }
  else {
    output[3] = input_color[3];
  }
}

void InvertOperation::deinit_execution()
{
  input_value_program_ = nullptr;
  input_color_program_ = nullptr;
}

void InvertOperation::update_memory_buffer_partial(MemoryBuffer *output,
                                                   const rcti &area,
                                                   Span<MemoryBuffer *> inputs)
{
  for (BuffersIterator<float> it = output->iterate_with(inputs, area); !it.is_end(); ++it) {
    const float value = *it.in(0);
    const float inverted_value = 1.0f - value;
    const float *color = it.in(1);

    if (color_) {
      it.out[0] = (1.0f - color[0]) * value + color[0] * inverted_value;
      it.out[1] = (1.0f - color[1]) * value + color[1] * inverted_value;
      it.out[2] = (1.0f - color[2]) * value + color[2] * inverted_value;
    }
    else {
      copy_v3_v3(it.out, color);
    }

    if (alpha_) {
      it.out[3] = (1.0f - color[3]) * value + color[3] * inverted_value;
    }
    else {
      it.out[3] = color[3];
    }
  }
}

}  // namespace blender::compositor
