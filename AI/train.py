"""
Image-Adaptive 3D LUT — Paired Training (PyTorch 2.x, optimized for GPU)
Based on: https://github.com/HuiZeng/Image-Adaptive-3DLUT
"""
import argparse
import os
import sys
import math
import time
import datetime
import itertools

import torch
import torch.nn as nn
from torch.utils.data import DataLoader, Dataset
from torchvision.utils import save_image
from PIL import Image
import torchvision.transforms as transforms
import torchvision.transforms.functional as TF
import numpy as np

from models_pytorch2 import *


class FiveKDataset(Dataset):
    """FiveK sRGB dataset with fixed-size output for batched training."""

    def __init__(self, root, mode="train", crop_size=256):
        self.mode = mode
        self.crop_size = crop_size

        train_input = sorted(open(os.path.join(root, 'train_input.txt')).read().splitlines())
        train_label = sorted(open(os.path.join(root, 'train_label.txt')).read().splitlines())
        test_ids = sorted(open(os.path.join(root, 'test.txt')).read().splitlines())

        all_train = train_input + train_label

        def make_paths(ids):
            inputs = [os.path.join(root, "input", "JPG", "480p", f"{x}.jpg") for x in ids]
            targets = [os.path.join(root, "expertC", "JPG", "480p", f"{x}.jpg") for x in ids]
            return inputs, targets

        if mode == "train":
            self.inputs, self.targets = make_paths(all_train)
        else:
            self.inputs, self.targets = make_paths(test_ids)

    def __len__(self):
        return len(self.inputs)

    def __getitem__(self, idx):
        img_input = Image.open(self.inputs[idx]).convert('RGB')
        img_target = Image.open(self.targets[idx]).convert('RGB')
        name = os.path.basename(self.inputs[idx])

        if self.mode == "train":
            ratio_H = np.random.uniform(0.6, 1.0)
            ratio_W = np.random.uniform(0.6, 1.0)
            W, H = img_input.size
            crop_h = max(round(H * ratio_H), self.crop_size)
            crop_w = max(round(W * ratio_W), self.crop_size)
            crop_h = min(crop_h, H)
            crop_w = min(crop_w, W)
            i, j, h, w = transforms.RandomCrop.get_params(img_input, output_size=(crop_h, crop_w))
            img_input = TF.crop(img_input, i, j, h, w)
            img_target = TF.crop(img_target, i, j, h, w)

            img_input = TF.resize(img_input, (self.crop_size, self.crop_size), antialias=True)
            img_target = TF.resize(img_target, (self.crop_size, self.crop_size), antialias=True)

            if np.random.random() > 0.5:
                img_input = TF.hflip(img_input)
                img_target = TF.hflip(img_target)
            img_input = TF.adjust_brightness(img_input, np.random.uniform(0.8, 1.2))
            img_input = TF.adjust_saturation(img_input, np.random.uniform(0.8, 1.2))
        else:
            img_input = TF.resize(img_input, (self.crop_size, self.crop_size), antialias=True)
            img_target = TF.resize(img_target, (self.crop_size, self.crop_size), antialias=True)

        return {
            "A_input": TF.to_tensor(img_input),
            "A_exptC": TF.to_tensor(img_target),
            "input_name": name,
        }


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--epoch", type=int, default=0)
    parser.add_argument("--n_epochs", type=int, default=400)
    parser.add_argument("--data_root", type=str,
                        default=r"C:\Users\THINKSTATION\Desktop\data\fiveK\fiveK")
    parser.add_argument("--batch_size", type=int, default=16)
    parser.add_argument("--crop_size", type=int, default=256,
                        help="fixed crop size for batched training")
    parser.add_argument("--lr", type=float, default=1e-4)
    parser.add_argument("--b1", type=float, default=0.9)
    parser.add_argument("--b2", type=float, default=0.999)
    parser.add_argument("--lambda_smooth", type=float, default=0.0001)
    parser.add_argument("--lambda_monotonicity", type=float, default=10.0)
    parser.add_argument("--n_cpu", type=int, default=4)
    parser.add_argument("--checkpoint_interval", type=int, default=10)
    parser.add_argument("--output_dir", type=str,
                        default="LUTs/paired/fiveK_480p_3LUT_sm_1e-4_mn_10_sRGB")
    return parser.parse_args()


def main():
    opt = parse_args()
    print(opt)

    SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
    SAVE_DIR = os.path.join(SCRIPT_DIR, "saved_models", opt.output_dir)
    os.makedirs(SAVE_DIR, exist_ok=True)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device: {device}", end="")
    if device.type == "cuda":
        print(f" — {torch.cuda.get_device_name(0)}")
    else:
        print()

    # ── Models ──────────────────────────────────────────────────────────
    LUT0 = Generator3DLUT_identity().to(device)
    LUT1 = Generator3DLUT_zero().to(device)
    LUT2 = Generator3DLUT_zero().to(device)
    classifier = Classifier().to(device)

    criterion_pixelwise = nn.MSELoss().to(device)
    TV3 = TV_3D().to(device)
    TV3.weight_r = TV3.weight_r.to(device)
    TV3.weight_g = TV3.weight_g.to(device)
    TV3.weight_b = TV3.weight_b.to(device)

    trilinear_ = TrilinearInterpolation()

    if opt.epoch != 0:
        LUTs = torch.load(os.path.join(SAVE_DIR, f"LUTs_{opt.epoch}.pth"),
                          map_location=device, weights_only=True)
        LUT0.load_state_dict(LUTs["0"])
        LUT1.load_state_dict(LUTs["1"])
        LUT2.load_state_dict(LUTs["2"])
        classifier.load_state_dict(
            torch.load(os.path.join(SAVE_DIR, f"classifier_{opt.epoch}.pth"),
                       map_location=device, weights_only=True))
    else:
        classifier.apply(weights_init_normal_classifier)
        torch.nn.init.constant_(classifier.model[16].bias.data, 1.0)

    # ── Optimizer ───────────────────────────────────────────────────────
    optimizer_G = torch.optim.Adam(
        itertools.chain(classifier.parameters(),
                        LUT0.parameters(), LUT1.parameters(), LUT2.parameters()),
        lr=opt.lr, betas=(opt.b1, opt.b2))

    # ── Data ────────────────────────────────────────────────────────────
    train_ds = FiveKDataset(opt.data_root, mode="train", crop_size=opt.crop_size)
    test_ds = FiveKDataset(opt.data_root, mode="test", crop_size=opt.crop_size)

    dataloader = DataLoader(train_ds, batch_size=opt.batch_size,
                            shuffle=True, num_workers=opt.n_cpu,
                            pin_memory=True, persistent_workers=True)
    psnr_dataloader = DataLoader(test_ds, batch_size=opt.batch_size,
                                 shuffle=False, num_workers=2,
                                 pin_memory=True, persistent_workers=True)

    print(f"Train: {len(train_ds)} images ({len(dataloader)} batches × {opt.batch_size}), "
          f"Test: {len(test_ds)} images")

    # ── Helpers ─────────────────────────────────────────────────────────
    def generator_train(img):
        pred = classifier(img).squeeze()
        if len(pred.shape) == 1:
            pred = pred.unsqueeze(0)
        gen_A0 = LUT0(img)
        gen_A1 = LUT1(img)
        gen_A2 = LUT2(img)
        weights_norm = torch.mean(pred ** 2)
        combine_A = (pred[:, 0:1, None, None] * gen_A0 +
                     pred[:, 1:2, None, None] * gen_A1 +
                     pred[:, 2:3, None, None] * gen_A2)
        return combine_A, weights_norm

    def generator_eval(img):
        pred = classifier(img).squeeze()
        if len(pred.shape) == 1:
            pred = pred.unsqueeze(0)
        LUT = pred[0, 0] * LUT0.LUT + pred[0, 1] * LUT1.LUT + pred[0, 2] * LUT2.LUT
        weights_norm = torch.mean(pred ** 2)
        _, combine_A = trilinear_(LUT, img)
        return combine_A, weights_norm

    @torch.no_grad()
    def calculate_psnr():
        classifier.eval()
        total_psnr = 0
        count = 0
        for batch in psnr_dataloader:
            real_A = batch["A_input"].to(device)
            real_B = batch["A_exptC"].to(device)
            for k in range(real_A.size(0)):
                fake_B, _ = generator_eval(real_A[k:k+1])
                fake_B = torch.round(fake_B * 255)
                real_Bk = torch.round(real_B[k:k+1] * 255)
                mse = criterion_pixelwise(fake_B, real_Bk)
                psnr = 10 * math.log10(255.0 * 255.0 / max(mse.item(), 1e-10))
                total_psnr += psnr
                count += 1
        return total_psnr / count

    # ── Training Loop ───────────────────────────────────────────────────
    prev_time = time.time()
    max_psnr = 0
    max_epoch = 0
    epoch_start = time.time()

    for epoch in range(opt.epoch, opt.n_epochs):
        mse_avg = 0
        psnr_avg = 0
        classifier.train()
        t_epoch = time.time()

        for i, batch in enumerate(dataloader):
            real_A = batch["A_input"].to(device, non_blocking=True)
            real_B = batch["A_exptC"].to(device, non_blocking=True)

            optimizer_G.zero_grad(set_to_none=True)
            fake_B, weights_norm = generator_train(real_A)
            mse = criterion_pixelwise(fake_B, real_B)

            tv0, mn0 = TV3(LUT0)
            tv1, mn1 = TV3(LUT1)
            tv2, mn2 = TV3(LUT2)
            tv_cons = tv0 + tv1 + tv2
            mn_cons = mn0 + mn1 + mn2

            loss = mse + opt.lambda_smooth * (weights_norm + tv_cons) + opt.lambda_monotonicity * mn_cons

            psnr_avg += 10 * math.log10(1 / max(mse.item(), 1e-10))
            mse_avg += mse.item()

            loss.backward()
            optimizer_G.step()

            batches_done = epoch * len(dataloader) + i
            batches_left = opt.n_epochs * len(dataloader) - batches_done
            time_left = datetime.timedelta(seconds=batches_left * (time.time() - prev_time))
            prev_time = time.time()

            sys.stdout.write(
                "\r[Epoch %d/%d] [Batch %d/%d] [psnr: %.2f, tv: %.4f, mn: %.4f] ETA: %s"
                % (epoch, opt.n_epochs, i + 1, len(dataloader),
                   psnr_avg / (i + 1), tv_cons, mn_cons, time_left))

        epoch_sec = time.time() - t_epoch
        avg_psnr = calculate_psnr()
        if avg_psnr > max_psnr:
            max_psnr = avg_psnr
            max_epoch = epoch
        sys.stdout.write(" [PSNR: %.2f] [best: %.2f@%d] (%.1fs)\n"
                         % (avg_psnr, max_psnr, max_epoch, epoch_sec))

        if epoch % opt.checkpoint_interval == 0 or epoch == opt.n_epochs - 1:
            LUTs = {"0": LUT0.state_dict(), "1": LUT1.state_dict(), "2": LUT2.state_dict()}
            torch.save(LUTs, os.path.join(SAVE_DIR, f"LUTs_{epoch}.pth"))
            torch.save(classifier.state_dict(), os.path.join(SAVE_DIR, f"classifier_{epoch}.pth"))
            with open(os.path.join(SAVE_DIR, "result.txt"), 'a') as f:
                f.write(f"[epoch {epoch}] PSNR: {avg_psnr:.2f} | best: {max_psnr:.2f}@{max_epoch}\n")

    total = time.time() - epoch_start
    print(f"\nDone! Total: {datetime.timedelta(seconds=int(total))}, "
          f"Best PSNR: {max_psnr:.2f} @ epoch {max_epoch}")


if __name__ == '__main__':
    main()
