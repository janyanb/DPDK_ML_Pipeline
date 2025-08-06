#ifndef ONNX_INFERENCE_H
#define ONNX_INFERENCE_H


#include "main.h"

int onnx_init(const char *model_path);
int onnx_infer(struct ml_input_features *features, float *result);
void onnx_cleanup();

#endif