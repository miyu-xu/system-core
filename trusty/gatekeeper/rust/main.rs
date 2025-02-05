//
// Copyright (C) 2022 The Android Open-Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! This module implements the HAL service for Gatekeeper (Rust) in Trusty.
use clap::Parser;
use gk_hal::channel::SerializedChannel;
use log::{error, info};
use std::{
    ffi::CString,
    panic,
    sync::{Arc, Mutex},
};
use trusty::DEFAULT_DEVICE;

/// Tipc port name for communiating with the TA.
const TRUSTY_GATEKEEPER_SERVICE_NAME: &str = "com.android.trusty.gatekeeper";

/// HAL service name.
static GK_SERVICE_NAME: &str = "android.hardware.gatekeeper.IGatekeeper";

/// HAL instance name.
static SERVICE_INSTANCE: &str = "default";

/// Local error type for failures in the HAL service.
#[derive(Debug, Clone)]
struct HalServiceError(String);

#[derive(Debug)]
struct TipcChannel(trusty::TipcChannel);

impl SerializedChannel for TipcChannel {
    const MAX_SIZE: usize = 4000;
    fn execute(&mut self, serialized_req: &[u8]) -> binder::Result<Vec<u8>> {
        self.0.send(serialized_req).map_err(|e| {
            binder::Status::new_exception(
                binder::ExceptionCode::TRANSACTION_FAILED,
                Some(&CString::new(format!("tipc send failed: {e:?}")).unwrap()),
            )
        })?;
        let mut rsp_data = Vec::new();
        self.0.recv(&mut rsp_data).map_err(|e| {
            binder::Status::new_exception(
                binder::ExceptionCode::TRANSACTION_FAILED,
                Some(&CString::new(format!("tipc recv failed: {e:?}")).unwrap()),
            )
        })?;
        Ok(rsp_data)
    }
}

#[derive(Parser, Debug)]
struct Args {
    /// Tipc device path
    #[arg(short, long, default_value_t = DEFAULT_DEVICE.to_string())]
    dev: String,
}

fn main() {
    if let Err(HalServiceError(e)) = inner_main() {
        panic!("HAL service failed: {:?}", e);
    }
}

fn inner_main() -> Result<(), HalServiceError> {
    let args = Args::parse();
    // Initialize Android logging.
    android_logger::init_once(
        android_logger::Config::default()
            .with_tag("gatekeeper-hal-trusty")
            .with_max_level(log::LevelFilter::Info)
            .with_log_buffer(android_logger::LogId::System),
    );
    // Redirect panic messages to logcat.
    panic::set_hook(Box::new(|panic_info| {
        error!("{}", panic_info);
    }));

    info!("Trusty Gatekeeper HAL service is starting.");

    info!("Starting thread pool");
    binder::ProcessState::start_thread_pool();

    // Create connection to the TA
    let connection = trusty::TipcChannel::connect(
        args.dev.as_str(),
        TRUSTY_GATEKEEPER_SERVICE_NAME,
    )
    .map_err(|e| {
        HalServiceError(
            format!("Failed to connect to Trusty Gatekeeper TA at {}: {e:?}", args.dev,),
        )
    })?;
    let tipc_channel = Arc::new(Mutex::new(TipcChannel(connection)));

    // Register the Gatekeeper service
    let gk_service = gk_hal::GatekeeperService::new_as_binder(tipc_channel);
    let service_name = format!("{GK_SERVICE_NAME}/{SERVICE_INSTANCE}");
    binder::add_service(&service_name, gk_service.as_binder()).map_err(|e| {
        HalServiceError(format!("Failed to register service {service_name}: {e:?}",))
    })?;

    info!("Successfully registered Gatekeeper HAL service");
    binder::ProcessState::join_thread_pool();
    info!("Gatekeeper HAL service terminating"); // should not reach here
    Ok(())
}
