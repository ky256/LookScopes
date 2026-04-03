import torch
import torch.nn as nn
import torch.nn.functional as F
import numpy as np
import os

def weights_init_normal_classifier(m):
    classname = m.__class__.__name__
    if classname.find("Conv") != -1:
        torch.nn.init.xavier_normal_(m.weight.data)
    elif classname.find("BatchNorm2d") != -1 or classname.find("InstanceNorm2d") != -1:
        torch.nn.init.normal_(m.weight.data, 1.0, 0.02)
        torch.nn.init.constant_(m.bias.data, 0.0)


def discriminator_block(in_filters, out_filters, normalization=False):
    layers = [nn.Conv2d(in_filters, out_filters, 3, stride=2, padding=1)]
    layers.append(nn.LeakyReLU(0.2))
    if normalization:
        layers.append(nn.InstanceNorm2d(out_filters, affine=True))
    return layers


class Classifier(nn.Module):
    def __init__(self, in_channels=3):
        super(Classifier, self).__init__()
        self.model = nn.Sequential(
            nn.Upsample(size=(256, 256), mode='bilinear', align_corners=False),
            nn.Conv2d(3, 16, 3, stride=2, padding=1),
            nn.LeakyReLU(0.2),
            nn.InstanceNorm2d(16, affine=True),
            *discriminator_block(16, 32, normalization=True),
            *discriminator_block(32, 64, normalization=True),
            *discriminator_block(64, 128, normalization=True),
            *discriminator_block(128, 128),
            nn.Dropout(p=0.5),
            nn.Conv2d(128, 3, 8, padding=0),
        )

    def forward(self, img_input):
        return self.model(img_input)


class TrilinearInterpolation(nn.Module):
    """Pure PyTorch trilinear interpolation using F.grid_sample (no CUDA extension needed)."""

    def forward(self, lut, img):
        # lut: (3, dim, dim, dim) — indexed as [output_channel, R_in, G_in, B_in]
        # img: (batch, 3, H, W) — pixel values in [0, 1]
        batch = img.size(0)

        # Build 3D sampling grid from pixel RGB values
        r = img[:, 0:1, :, :]  # (batch, 1, H, W)
        g = img[:, 1:2, :, :]
        b = img[:, 2:3, :, :]

        # LUT axes: [channel, B_idx, G_idx, R_idx] → D=B, H=G, W=R
        # grid_sample maps: x→W(R), y→H(G), z→D(B)
        grid = torch.cat([r * 2 - 1, g * 2 - 1, b * 2 - 1], dim=1)  # (batch, 3, H, W)
        grid = grid.permute(0, 2, 3, 1).unsqueeze(1)                  # (batch, 1, H, W, 3)

        # Expand LUT to batch: (1, 3, dim, dim, dim) → (batch, 3, dim, dim, dim)
        lut_5d = lut.unsqueeze(0).expand(batch, -1, -1, -1, -1)

        output = F.grid_sample(lut_5d, grid, mode='bilinear', padding_mode='border', align_corners=True)
        output = output[:, :, 0, :, :]  # (batch, 3, H, W)

        return lut, output


class Generator3DLUT_identity(nn.Module):
    def __init__(self, dim=33):
        super(Generator3DLUT_identity, self).__init__()
        file_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), f"IdentityLUT{dim}.txt")
        with open(file_path, 'r') as f:
            lines = f.readlines()
        buffer = np.zeros((3, dim, dim, dim), dtype=np.float32)
        for i in range(dim):
            for j in range(dim):
                for k in range(dim):
                    n = i * dim * dim + j * dim + k
                    x = lines[n].split()
                    buffer[0, i, j, k] = float(x[0])
                    buffer[1, i, j, k] = float(x[1])
                    buffer[2, i, j, k] = float(x[2])
        self.LUT = nn.Parameter(torch.from_numpy(buffer).requires_grad_(True))
        self.TrilinearInterpolation = TrilinearInterpolation()

    def forward(self, x):
        _, output = self.TrilinearInterpolation(self.LUT, x)
        return output


class Generator3DLUT_zero(nn.Module):
    def __init__(self, dim=33):
        super(Generator3DLUT_zero, self).__init__()
        self.LUT = nn.Parameter(torch.zeros(3, dim, dim, dim, dtype=torch.float))
        self.TrilinearInterpolation = TrilinearInterpolation()

    def forward(self, x):
        _, output = self.TrilinearInterpolation(self.LUT, x)
        return output


class TV_3D(nn.Module):
    def __init__(self, dim=33):
        super(TV_3D, self).__init__()
        self.weight_r = torch.ones(3, dim, dim, dim - 1, dtype=torch.float)
        self.weight_r[:, :, :, (0, dim - 2)] *= 2.0
        self.weight_g = torch.ones(3, dim, dim - 1, dim, dtype=torch.float)
        self.weight_g[:, :, (0, dim - 2), :] *= 2.0
        self.weight_b = torch.ones(3, dim - 1, dim, dim, dtype=torch.float)
        self.weight_b[:, (0, dim - 2), :, :] *= 2.0
        self.relu = nn.ReLU()

    def forward(self, LUT):
        dif_r = LUT.LUT[:, :, :, :-1] - LUT.LUT[:, :, :, 1:]
        dif_g = LUT.LUT[:, :, :-1, :] - LUT.LUT[:, :, 1:, :]
        dif_b = LUT.LUT[:, :-1, :, :] - LUT.LUT[:, 1:, :, :]
        tv = (torch.mean(torch.mul((dif_r ** 2), self.weight_r)) +
              torch.mean(torch.mul((dif_g ** 2), self.weight_g)) +
              torch.mean(torch.mul((dif_b ** 2), self.weight_b)))
        mn = (torch.mean(self.relu(dif_r)) +
              torch.mean(self.relu(dif_g)) +
              torch.mean(self.relu(dif_b)))
        return tv, mn
