import torch
import random
import numpy as np
import matplotlib.pyplot as plt


def function_bias_gelu(input, bias):
    input = torch.as_tensor(input)
    bias = torch.as_tensor(bias)
    bias_gelu = torch.nn.functional.gelu(torch.add(input, bias))
    return bias_gelu


def custom_bias_gelu(input, bias):
    result = gelu(input + bias)
    return result


def gelu(x):
    return (
        0.5
        * x
        * (1.0 + np.tanh(np.sqrt(2.0 / np.pi) * (x + 0.044715 * np.power(x, 3))))
    )


x = np.linspace(-1e-10, 1e10, 500)
y = np.linspace(-1e-10, 1e10, 500)
z = function_bias_gelu(x, y)
z1 = custom_bias_gelu(x, y)
plt.plot(x, z, "--g", label="bias_gelu")
plt.plot(x, z1, "+r", label="custom bias_gelu")
plt.legend(loc="upper center")
plt.show()
