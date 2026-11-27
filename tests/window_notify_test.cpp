// /notify (game/window_notify.h): the line's grammar (MQ's notifications, the 1-based
// row/page, Sentinel's AutoAccept trailing guard) and the headless fallback's action table,
// whose ids are the StockWindows' own.

#include <doctest/doctest.h>

#include "game/window_notify.h"

using namespace hailstone::game;

TEST_CASE("window notify: parse") {
    {
        const WindowNotify n = parse_window_notify("TradeWnd TRDW_Trade_Button leftmouseup");
        CHECK(n.error.empty());
        CHECK(n.window == "TradeWnd");
        CHECK(n.child == "TRDW_Trade_Button");
        CHECK(n.notification == "leftmouseup");
        CHECK(n.value == 0);
        CHECK(n.expected.empty());
    }
    {
        // AutoAccept's guarded click: the requester's name trails the notification.
        const WindowNotify n = parse_window_notify("ConfirmationDialogBox Yes_Button leftmouseup Friend");
        CHECK(n.error.empty());
        CHECK(n.expected == "Friend");
    }
    {
        // Quoted words are GetNextCmdArg's: a name with a space stays one guard word.
        const WindowNotify n = parse_window_notify("TradeWnd TRDW_Trade_Button leftmouseup 'Some One'");
        CHECK(n.error.empty());
        CHECK(n.expected == "Some One");
    }
    {
        const WindowNotify n = parse_window_notify("AAWindow AAL_List listselect 3");
        CHECK(n.error.empty());
        CHECK(n.value == 3);
        const WindowNotify tab = parse_window_notify("OptionsWindow OPTW_OptionsSubwindows TABSELECT 5");
        CHECK(tab.error.empty());
        CHECK(tab.value == 5);
    }
    // Malformed lines name the usage, not a guess.
    CHECK(!parse_window_notify("").error.empty());
    CHECK(!parse_window_notify("TradeWnd").error.empty());
    CHECK(!parse_window_notify("TradeWnd TRDW_Trade_Button").error.empty());
    CHECK(!parse_window_notify("TradeWnd TRDW_Trade_Button leftclick").error.empty());
    CHECK(!parse_window_notify("AAWindow AAL_List listselect").error.empty());
    CHECK(!parse_window_notify("AAWindow AAL_List listselect 0").error.empty());
    CHECK(!parse_window_notify("AAWindow AAL_List listselect first").error.empty());
    CHECK(!parse_window_notify("TradeWnd TRDW_Trade_Button leftmouseup a b").error.empty());
    CHECK(!parse_window_notify("TradeWnd TRDW_Trade_Button leftmouseup a b c").error.empty());
}

TEST_CASE("window notify: the headless action table") {
    CHECK(notify_action("TradeWnd", "TRDW_Trade_Button") == NotifyAction::TradeAccept);
    CHECK(notify_action("tradewnd", "trdw_cancel_button") == NotifyAction::TradeCancel);
    CHECK(notify_action("LootWnd", "LootAllButton") == NotifyAction::LootAll);
    CHECK(notify_action("LootWnd", "DoneButton") == NotifyAction::LootDone);
    CHECK(notify_action("ConfirmationDialogBox", "Yes_Button") == NotifyAction::ConfirmYes);
    CHECK(notify_action("ConfirmationDialogBox", "OK_Button") == NotifyAction::ConfirmYes);
    CHECK(notify_action("ConfirmationDialogBox", "No_Button") == NotifyAction::ConfirmNo);
    CHECK(notify_action("LargeDialogWindow", "LDW_OkButton") == NotifyAction::LargeDialogOk);
    CHECK(notify_action("LargeDialogWindow", "LDW_YesButton") == NotifyAction::LargeDialogYes);
    CHECK(notify_action("LargeDialogWindow", "LDW_NoButton") == NotifyAction::LargeDialogNo);
    CHECK(notify_action("TaskSelectWnd", "DeclineButton") == NotifyAction::TaskDecline);
    // Accept acts on the window's selection, which is the UI's state, not the connection's.
    CHECK(notify_action("TaskSelectWnd", "AcceptButton") == NotifyAction::None);
    CHECK(notify_action("BankWnd", "AutoButton") == NotifyAction::BankAuto);
    CHECK(notify_action("BankWnd", "DoneButton") == NotifyAction::BankDone);
    CHECK(notify_action("BigBankWnd", "AutoButton") == NotifyAction::BankAuto);
    CHECK(notify_action("BigBankWnd", "DoneButton") == NotifyAction::BankDone);
    CHECK(notify_action("TributeBenefitWnd", "ActivateButton") == NotifyAction::TributeToggle);
    CHECK(notify_action("TributeBenefitWnd", "GuildActivateButton") == NotifyAction::GuildTributeToggle);
    // Windows and children the table does not name need the UI.
    CHECK(notify_action("TradeWnd", "TRDW_TradeSlot0") == NotifyAction::None);
    CHECK(notify_action("InventoryWindow", "DoneButton") == NotifyAction::None);
    CHECK(notify_action("", "") == NotifyAction::None);
}
