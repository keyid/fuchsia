// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::{format_err, Error};
use fidl::endpoints::{ClientEnd, ServerEnd};
use fidl_fuchsia_auth::AuthStateSummary;
use fidl_fuchsia_auth_account::{
    AccountManagerMarker, AccountManagerProxy, AccountProxy, LocalAccountId, Status,
};
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::prelude::*;

/// Executes the supplied test function against a connected AccountManagerProxy.
fn proxy_test<TestFn, Fut>(test_fn: TestFn)
where
    TestFn: FnOnce(AccountManagerProxy) -> Fut,
    Fut: Future<Output = Result<(), Error>>,
{
    let mut executor = fasync::Executor::new().expect("Failed to create executor");
    let proxy = fuchsia_app::client::connect_to_service::<AccountManagerMarker>()
        .expect("Failed to connect to account manager service");;

    executor
        .run_singlethreaded(test_fn(proxy))
        .expect("Executor run failed.")
}

/// Calls provision_new_account on the supplied account_manager, returning an error on any
/// non-OK responses, or the account ID on success.
async fn provision_new_account(
    account_manager: &AccountManagerProxy,
) -> Result<LocalAccountId, Error> {
    match await!(account_manager.provision_new_account())? {
        (Status::Ok, Some(new_account_id)) => Ok(*new_account_id),
        (status, _) => Err(format_err!(
            "ProvisionNewAccount returned status: {:?}",
            status
        )),
    }
}

// TODO(jsankey): Work with ComponentFramework and cramertj@ to develop a nice Rust equivalent of
// the C++ TestWithEnvironment fixture to provide isolated environments for each test case. For now
// we verify all functionality in a single test case.
#[test]
fn test_account_functionality() {
    proxy_test(async move |account_manager| {
        // Verify we initially have no accounts.
        assert_eq!(await!(account_manager.get_account_ids())?, vec![]);

        // Provision a new account.
        let mut account_1 = await!(provision_new_account(&account_manager))?;
        assert_eq!(
            await!(account_manager.get_account_ids())?,
            vec![LocalAccountId { id: account_1.id }]
        );

        // Provision a second new account and verify it has a different ID.
        let mut account_2 = await!(provision_new_account(&account_manager))?;
        assert_ne!(account_1.id, account_2.id);

        // Connect a channel to one of these accounts and verify it's usable.
        let (_, acp_client_chan) = zx::Channel::create()?;
        let (account_server_chan, account_client_chan) = zx::Channel::create()?;
        assert_eq!(
            await!(account_manager.get_account(
                &mut account_1,
                ClientEnd::new(acp_client_chan),
                ServerEnd::new(account_server_chan)
            ))?,
            Status::Ok
        );
        let account =
            AccountProxy::new(fasync::Channel::from_channel(account_client_chan).unwrap());
        let account_auth_state = match await!(account.get_auth_state())? {
            (Status::Ok, Some(auth_state)) => *auth_state,
            (status, _) => return Err(format_err!("GetAuthState returned status: {:?}", status)),
        };
        assert_eq!(account_auth_state.summary, AuthStateSummary::Unknown);

        // Connect a channel to the account's default persona and verify it's usable.
        let (persona_server_chan, persona_client_chan) = zx::Channel::create()?;
        assert_eq!(
            await!(account.get_default_persona(ServerEnd::new(persona_server_chan)))?.0,
            Status::Ok
        );
        let persona =
            AccountProxy::new(fasync::Channel::from_channel(persona_client_chan).unwrap());
        let persona_auth_state = match await!(persona.get_auth_state())? {
            (Status::Ok, Some(auth_state)) => *auth_state,
            (status, _) => return Err(format_err!("GetAuthState returned status: {:?}", status)),
        };
        assert_eq!(persona_auth_state.summary, AuthStateSummary::Unknown);

        // Delete both accounts and verify they are removed.
        assert_eq!(
            await!(account_manager.remove_account(&mut account_1))?,
            Status::Ok
        );
        assert_eq!(
            await!(account_manager.remove_account(&mut account_2))?,
            Status::Ok
        );
        assert_eq!(await!(account_manager.get_account_ids())?, vec![]);

        Ok(())
    });
}