// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use hex;

use failure::Fail;

#[derive(Fail, Debug, PartialEq)]
pub enum BlobIdParseError {
    #[fail(display = "cannot contain uppercase hex characters")]
    CannotContainUppercase,

    #[fail(display = "invalid length, expected 32 hex bytes, got {}", _0)]
    InvalidLength(usize),

    #[fail(display = "{}", _0)]
    FromHexError(#[cause] hex::FromHexError),
}

impl From<hex::FromHexError> for BlobIdParseError {
    fn from(err: hex::FromHexError) -> Self {
        BlobIdParseError::FromHexError(err)
    }
}