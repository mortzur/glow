from __future__ import absolute_import, division, print_function, unicode_literals

import torch
import torch_glow

from tests.utils import jitVsGlow


def test_sigmoid_basic():
    """Basic test of the PyTorch sigmoid Node on Glow"""
    def sigmoid_basic(a):
        c = a + a
        return c.sigmoid()

    x = torch.randn(6)

    jitVsGlow(sigmoid_basic, x, expected_fused_ops={"aten::sigmoid"})


def test_sigmoid_inplace():
    """Test of the inplace PyTorch sigmoid Node on Glow"""
    def sigmoid_inplace(a):
        c = a + a
        return c.sigmoid_()

    x = torch.randn(6)

    jitVsGlow(sigmoid_inplace, x, expected_fused_ops={"aten::sigmoid_"})
