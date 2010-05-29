// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Utility for manipulating verified boot kernel images.
//

#include "kernel_utility.h"

#include <getopt.h>
#include <stdio.h>
#include <stdint.h>  // Needed for UINT16_MAX.
#include <stdlib.h>
#include <unistd.h>

#include <iostream>

extern "C" {
#include "cryptolib.h"
#include "file_keys.h"
#include "kernel_image.h"
#include "stateful_util.h"
}

using std::cerr;

// Macro to determine the size of a field structure in the KernelImage
// structure.
#define FIELD_LEN(field) (sizeof(((KernelImage*)0)->field))

namespace vboot_reference {

KernelUtility::KernelUtility(): image_(NULL),
                                firmware_key_pub_(NULL),
                                header_version_(1),
                                firmware_sign_algorithm_(-1),
                                kernel_sign_algorithm_(-1),
                                kernel_key_version_(-1),
                                kernel_version_(-1),
                                padding_(0),
                                kernel_len_(0),
                                is_generate_(false),
                                is_verify_(false),
                                is_describe_(false),
                                is_only_vblock_(false) {}

KernelUtility::~KernelUtility() {
  RSAPublicKeyFree(firmware_key_pub_);
  KernelImageFree(image_);
}

void KernelUtility::PrintUsage(void) {
  cerr << "\n"
      "Utility to generate/verify/describe a verified boot kernel image\n"
      "\n"
      "Usage: kernel_utility <--generate|--verify|--describe> [OPTIONS]\n"
      "\n"
      "For \"--describe\", the required OPTIONS are:\n"
      "  --in <infile>\t\t\t\tSigned boot image to describe.\n"
      "\n"
      "For \"--verify\",  required OPTIONS are:\n"
      "  --in <infile>\t\t\t\tSigned boot image to verify.\n"
      "  --firmware_key_pub <pubkeyfile>\tPre-processed public firmware key\n"
      "\n"
      "For \"--generate\", required OPTIONS are:\n"
      "  --firmware_key <privkeyfile>\t\tPrivate firmware signing key file\n"
      "  --kernel_key_pub <pubkeyfile>\t\tPre-processed public kernel signing"
      " key\n"
      "  --firmware_sign_algorithm <algoid>\tSigning algorithm for firmware\n"
      "  --kernel_sign_algorithm <algoid>\tSigning algorithm for kernel\n"
      "  --kernel_key_version <number>\t\tKernel signing key version number\n"
      "OR\n"
      "  --subkey_in <subkeyfile>\t\tExisting key signature header\n"
      "\n"
      "  --kernel_key <privkeyfile>\t\tPrivate kernel signing key file\n"
      "  --kernel_version <number>\t\tKernel Version number\n"
      "  --config <file>\t\t\tEmbedded kernel command-line parameters\n"
      "  --bootloader <file>\t\t\tEmbedded bootloader stub\n"
      "  --vmlinuz <file>\t\t\tEmbedded kernel image\n"
      "  --out <outfile>\t\t\tOutput file for verified boot image\n"
      "\n"
      "Optional arguments for \"--generate\" are:\n"
      "  --padding <size>\t\t\tPad the header to this size\n"
      "  --subkey_out\t\t\t\tJust output the subkey (key verification) header\n"
      "  --vblock\t\t\t\tJust output the verification block\n"
      "\n"
      "<algoid> (for --*_sign_algorithm) is one of the following:\n";
  for (int i = 0; i < kNumAlgorithms; i++) {
    cerr << "  " << i << " for " << algo_strings[i] << "\n";
  }
  cerr << "\n\n";
}

bool KernelUtility::ParseCmdLineOptions(int argc, char* argv[]) {
  int option_index, i;
  char *e = 0;
  enum {
    OPT_FIRMWARE_KEY = 1000,
    OPT_FIRMWARE_KEY_PUB,
    OPT_KERNEL_KEY,
    OPT_KERNEL_KEY_PUB,
    OPT_SUBKEY_IN,
    OPT_FIRMWARE_SIGN_ALGORITHM,
    OPT_KERNEL_SIGN_ALGORITHM,
    OPT_KERNEL_KEY_VERSION,
    OPT_KERNEL_VERSION,
    OPT_IN,
    OPT_OUT,
    OPT_GENERATE,
    OPT_VERIFY,
    OPT_DESCRIBE,
    OPT_VBLOCK,
    OPT_BOOTLOADER,
    OPT_VMLINUZ,
    OPT_CONFIG,
    OPT_PADDING,
    OPT_SUBKEY_OUT,
  };
  static struct option long_options[] = {
    {"firmware_key", 1, 0,              OPT_FIRMWARE_KEY            },
    {"firmware_key_pub", 1, 0,          OPT_FIRMWARE_KEY_PUB        },
    {"kernel_key", 1, 0,                OPT_KERNEL_KEY              },
    {"kernel_key_pub", 1, 0,            OPT_KERNEL_KEY_PUB          },
    {"subkey_in", 1, 0,                 OPT_SUBKEY_IN               },
    {"firmware_sign_algorithm", 1, 0,   OPT_FIRMWARE_SIGN_ALGORITHM },
    {"kernel_sign_algorithm", 1, 0,     OPT_KERNEL_SIGN_ALGORITHM   },
    {"kernel_key_version", 1, 0,        OPT_KERNEL_KEY_VERSION      },
    {"kernel_version", 1, 0,            OPT_KERNEL_VERSION          },
    {"in", 1, 0,                        OPT_IN                      },
    {"out", 1, 0,                       OPT_OUT                     },
    {"generate", 0, 0,                  OPT_GENERATE                },
    {"verify", 0, 0,                    OPT_VERIFY                  },
    {"describe", 0, 0,                  OPT_DESCRIBE                },
    {"vblock", 0, 0,                    OPT_VBLOCK                  },
    {"bootloader", 1, 0,                OPT_BOOTLOADER              },
    {"vmlinuz", 1, 0,                   OPT_VMLINUZ                 },
    {"config", 1, 0,                    OPT_CONFIG                  },
    {"padding", 1, 0,                   OPT_PADDING                 },
    {"subkey_out", 0, 0,                OPT_SUBKEY_OUT              },
    {NULL, 0, 0, 0}
  };
  while ((i = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
    switch (i) {
    case '?':
      return false;
      break;
    case OPT_FIRMWARE_KEY:
      firmware_key_file_ = optarg;
      break;
    case OPT_FIRMWARE_KEY_PUB:
      firmware_key_pub_file_ = optarg;
      break;
    case OPT_KERNEL_KEY:
      kernel_key_file_ = optarg;
      break;
    case OPT_KERNEL_KEY_PUB:
      kernel_key_pub_file_ = optarg;
      break;
    case OPT_SUBKEY_IN:
      subkey_in_file_ = optarg;
      break;
    case OPT_FIRMWARE_SIGN_ALGORITHM:
      firmware_sign_algorithm_ = strtol(optarg, &e, 0);
      if (!*optarg || (e && *e)) {
        cerr << "Invalid argument to --"
             << long_options[option_index].name
             << ": " << optarg << "\n";
        return false;
      }
      break;
    case OPT_KERNEL_SIGN_ALGORITHM:
      kernel_sign_algorithm_ = strtol(optarg, &e, 0);
      if (!*optarg || (e && *e)) {
        cerr << "Invalid argument to --"
             << long_options[option_index].name
             << ": " << optarg << "\n";
        return false;
      }
      break;
    case OPT_KERNEL_KEY_VERSION:
      kernel_key_version_ = strtol(optarg, &e, 0);
      if (!*optarg || (e && *e)) {
        cerr << "Invalid argument to --"
             << long_options[option_index].name
             << ": " << optarg << "\n";
        return false;
      }
      break;
    case OPT_KERNEL_VERSION:
      kernel_version_ = strtol(optarg, &e, 0);
      if (!*optarg || (e && *e)) {
        cerr << "Invalid argument to --"
             << long_options[option_index].name
             << ": " << optarg << "\n";
        return false;
      }
      break;
    case OPT_IN:
      in_file_ = optarg;
      break;
    case OPT_OUT:
      out_file_ = optarg;
      break;
    case OPT_GENERATE:
      is_generate_ = true;
      break;
    case OPT_VERIFY:
      is_verify_ = true;
      break;
    case OPT_DESCRIBE:
      is_describe_ = true;
      break;
    case OPT_VBLOCK:
      is_only_vblock_ = true;
      break;
    case OPT_BOOTLOADER:
      bootloader_file_ = optarg;
      break;
    case OPT_VMLINUZ:
      vmlinuz_file_ = optarg;
      break;
    case OPT_CONFIG:
      config_file_ = optarg;
      break;
    case OPT_PADDING:
      padding_ = strtol(optarg, &e, 0);
      if (!*optarg || (e && *e)) {
        cerr << "Invalid argument to --"
             << long_options[option_index].name
             << ": " << optarg << "\n";
        return false;
      }
      break;
    case OPT_SUBKEY_OUT:
      is_subkey_out_ = true;
      break;
    }
  }
  return CheckOptions();
}

void KernelUtility::OutputSignedImage(void) {
  if (image_) {
    if (!WriteKernelImage(out_file_.c_str(), image_,
                          is_only_vblock_,
                          is_subkey_out_)) {
      cerr << "Couldn't write verified boot kernel image to file "
           << out_file_ <<".\n";
    }
  }
}

void KernelUtility::DescribeSignedImage(void) {
  image_ = ReadKernelImage(in_file_.c_str());
  if (!image_) {
    cerr << "Couldn't read kernel image or malformed image.\n";
    return;
  }
  PrintKernelImage(image_);
}

bool KernelUtility::GenerateSignedImage(void) {
  uint64_t kernel_key_pub_len;

  image_ = KernelImageNew();
  Memcpy(image_->magic, KERNEL_MAGIC, KERNEL_MAGIC_SIZE);

  if (subkey_in_file_.empty()) {
    // We must generate the kernel key signature header (subkey header)
    // ourselves.
    image_->header_version = 1;
    image_->firmware_sign_algorithm = (uint16_t) firmware_sign_algorithm_;
    // Copy pre-processed public signing key.
    image_->kernel_sign_algorithm = (uint16_t) kernel_sign_algorithm_;
    image_->kernel_sign_key = BufferFromFile(kernel_key_pub_file_.c_str(),
                                             &kernel_key_pub_len);
    if (!image_->kernel_sign_key)
      return false;
    image_->kernel_key_version = kernel_key_version_;

    // Update header length.
    image_->header_len = GetKernelHeaderLen(image_);
    // Calculate header checksum.
    CalculateKernelHeaderChecksum(image_, image_->header_checksum);

    // Generate and add the key signatures.
    if (!AddKernelKeySignature(image_, firmware_key_file_.c_str())) {
      cerr << "Couldn't write key signature to verified boot kernel image.\n";
      return false;
    }
  } else {
    // Use existing subkey header.
    MemcpyState st;
    uint8_t* subkey_header_buf = NULL;
    uint64_t subkey_len;
    int header_len;
    int kernel_key_signature_len;
    int kernel_sign_key_len;
    uint8_t header_checksum[FIELD_LEN(header_checksum)];

    subkey_header_buf = BufferFromFile(subkey_in_file_.c_str(), &subkey_len);
    if (!subkey_header_buf) {
      cerr << "Couldn't read subkey header from file %s\n"
           << subkey_in_file_.c_str();
      return false;
    }
    st.remaining_len = subkey_len;
    st.remaining_buf = subkey_header_buf;
    st.overrun = 0;

    // TODO(gauravsh): This is basically the same code as the first half of
    // of ReadKernelImage(). Refactor to eliminate code duplication.

    StatefulMemcpy(&st, &image_->header_version, FIELD_LEN(header_version));
    StatefulMemcpy(&st, &image_->header_len, FIELD_LEN(header_len));
    StatefulMemcpy(&st, &image_->firmware_sign_algorithm,
                   FIELD_LEN(firmware_sign_algorithm));
    StatefulMemcpy(&st, &image_->kernel_sign_algorithm,
                   FIELD_LEN(kernel_sign_algorithm));

    // Valid Kernel Key signing algorithm.
    if (image_->firmware_sign_algorithm >= kNumAlgorithms) {
      Free(subkey_header_buf);
      return NULL;
    }

    // Valid Kernel Signing Algorithm?
    if (image_->kernel_sign_algorithm >= kNumAlgorithms) {
      Free(subkey_header_buf);
      return NULL;
    }

    // Compute size of pre-processed RSA public keys and signatures.
    kernel_key_signature_len  = siglen_map[image_->firmware_sign_algorithm];
    kernel_sign_key_len = RSAProcessedKeySize(image_->kernel_sign_algorithm);

    // Check whether key header length is correct.
    header_len = GetKernelHeaderLen(image_);
    if (header_len != image_->header_len) {
      debug("Header length mismatch. Got: %d, Expected: %d\n",
            image_->header_len, header_len);
      Free(subkey_header_buf);
      return NULL;
    }

    // Read pre-processed public half of the kernel signing key.
    StatefulMemcpy(&st, &image_->kernel_key_version,
                   FIELD_LEN(kernel_key_version));
    image_->kernel_sign_key = (uint8_t*) Malloc(kernel_sign_key_len);
    StatefulMemcpy(&st, image_->kernel_sign_key, kernel_sign_key_len);
    StatefulMemcpy(&st, image_->header_checksum, FIELD_LEN(header_checksum));

    // Check whether the header checksum matches.
    CalculateKernelHeaderChecksum(image_, header_checksum);
    if (SafeMemcmp(header_checksum, image_->header_checksum,
                   FIELD_LEN(header_checksum))) {
      debug("Invalid kernel header checksum!\n");
      Free(subkey_header_buf);
      return NULL;
    }

    // Read key signature.
    image_->kernel_key_signature = (uint8_t*) Malloc(kernel_key_signature_len);
    StatefulMemcpy(&st, image_->kernel_key_signature,
                   kernel_key_signature_len);

    Free(subkey_header_buf);
    if (st.overrun || st.remaining_len != 0)  /* Overrun or underrun. */
      return false;
    return true;
  }

  // Fill up kernel preamble and kernel data.
  image_->kernel_version = kernel_version_;
  if (padding_)
    image_->padded_header_size = padding_;
  image_->kernel_data = GenerateKernelBlob(vmlinuz_file_.c_str(),
                                           config_file_.c_str(),
                                           bootloader_file_.c_str(),
                                           &image_->kernel_len,
                                           &image_->bootloader_offset,
                                           &image_->bootloader_size);
  if (!image_->kernel_data)
    return false;

  // Generate and add the preamble and data signatures.
  if (!AddKernelSignature(image_, kernel_key_file_.c_str())) {
    cerr << "Couldn't write firmware signature to verified boot kernel image.\n";
    return false;
  }
  return true;
}

bool KernelUtility::VerifySignedImage(void) {
  int error;
  firmware_key_pub_ = RSAPublicKeyFromFile(firmware_key_pub_file_.c_str());
  image_ = ReadKernelImage(in_file_.c_str());

  if (!firmware_key_pub_) {
    cerr << "Couldn't read pre-processed public root key.\n";
    return false;
  }

  if (!image_) {
    cerr << "Couldn't read kernel image or malformed image.\n";
    return false;
  }
  if (!(error = VerifyKernelImage(firmware_key_pub_, image_, 0)))
    return true;
  cerr << VerifyKernelErrorString(error) << "\n";
  return false;
}

bool KernelUtility::CheckOptions(void) {
  // Ensure that only one of --{describe|generate|verify} is set.
  if (!((is_describe_ && !is_generate_ && !is_verify_) ||
        (!is_describe_ && is_generate_ && !is_verify_) ||
        (!is_describe_ && !is_generate_ && is_verify_))) {
    cerr << "One (and only one) of --describe, --generate or --verify "
         << "must be specified.\n";
    return false;
  }
  // Common required options.
  // Required options for --describe.
  if (is_describe_) {
    if (in_file_.empty()) {
      cerr << "No input file specified.\n";
      return false;
    }
  }
  // Required options for --verify.
  if (is_verify_) {
    if (firmware_key_pub_file_.empty()) {
      cerr << "No pre-processed public firmware key file specified.\n";
      return false;
    }
    if (in_file_.empty()) {
      cerr << "No input file specified.\n";
      return false;
    }
  }
  // Required options for --generate.
  if (is_generate_) {
    if (subkey_in_file_.empty()) {
      // Firmware private key (root key), kernel signing public
      // key, and signing algorithms are required to generate the  key signature
      // header.
      if (firmware_key_file_.empty()) {
        cerr << "No firmware key file specified.\n";
        return false;
      }
      if (kernel_key_pub_file_.empty()) {
        cerr << "No pre-processed public kernel key file specified\n";
        return false;
      }
      if (kernel_key_version_ <= 0 || kernel_key_version_ > UINT16_MAX) {
        cerr << "Invalid or no kernel key version specified.\n";
        return false;
      }
      if (firmware_sign_algorithm_ < 0 ||
          firmware_sign_algorithm_ >= kNumAlgorithms) {
        cerr << "Invalid or no firmware signing key algorithm specified.\n";
        return false;
      }
      if (kernel_sign_algorithm_ < 0 ||
          kernel_sign_algorithm_ >= kNumAlgorithms) {
        cerr << "Invalid or no kernel signing key algorithm specified.\n";
        return false;
      }
    }
    if (kernel_key_file_.empty()) {
      cerr << "No kernel key file specified.\n";
      return false;
    }
    if (kernel_version_ <=0 || kernel_version_ > UINT16_MAX) {
      cerr << "Invalid or no kernel version specified.\n";
      return false;
    }
    if (out_file_.empty()) {
      cerr <<"No output file specified.\n";
      return false;
    }
    if (config_file_.empty()) {
      cerr << "No config file specified.\n";
      return false;
    }
    if (bootloader_file_.empty()) {
      cerr << "No bootloader file specified.\n";
      return false;
    }
    if (vmlinuz_file_.empty()) {
      cerr << "No vmlinuz file specified.\n";
      return false;
    }
    // TODO(gauravsh): Enforce only one of --vblock or --subkey_out is specified
  }
  return true;
}

}  // namespace vboot_reference

int main(int argc, char* argv[]) {
  vboot_reference::KernelUtility ku;
  if (!ku.ParseCmdLineOptions(argc, argv)) {
    ku.PrintUsage();
    return -1;
  }
  if (ku.is_describe()) {
    ku.DescribeSignedImage();
  }
  else if (ku.is_generate()) {
    if (!ku.GenerateSignedImage())
      return -1;
    ku.OutputSignedImage();
  }
  else if (ku.is_verify()) {
    cerr << "Verification ";
    if (ku.VerifySignedImage())
      cerr << "SUCCESS.\n";
    else
      cerr << "FAILURE.\n";
  }
  return 0;
}
