#pragma once

// /notify: MacroQuest's window-notification command, which this client adds (it is not in the
// original's command table) so scripts written against MQ's WndNotify keep working:
//
//   /notify <window> <child> <notification> [<value>] [<expected>]
//
// With a UI the notification goes to the stock window as the player's click would (ClientApp,
// through UiApi). Without one (a headless client) the buttons whose handler is a single
// GameConnection call still work: notify_action below names them, and GameConnection carries
// them out behind the same state checks the window's own click runs under. Everything else
// needs the window (a list's selection, a text field) and is refused headless, never guessed.
//
// The notifications are MQ's names: leftmouseup / rightmouseup for a click, listselect with a
// 1-based row, tabselect with a 1-based page. A trailing word after a click (Sentinel's
// AutoAccept appends the character it expects: "... leftmouseup Friend") is a guard, not part
// of the notification: the fallback refuses to act when the trade's partner or the question's
// requester is not that character.

#include <string>
#include <string_view>

namespace hailstone::game {

// One parsed /notify line. `value` is the 1-based row/page of listselect/tabselect (0 when the
// line carries none); `expected` is the trailing guard word, empty when there is none. A
// malformed line sets `error` (a usage line for the player) and leaves the rest empty.
struct WindowNotify {
    std::string window;
    std::string child;
    std::string notification;
    int value = 0;
    std::string expected;
    std::string error;
};
WindowNotify parse_window_notify(std::string_view args);

// The headless fallback's vocabulary: the window buttons that are one connection call while
// their window shows. A window gains a headless /notify by adding its row here (its screen and
// child ids as its StockWindow names them) and its case in GameConnection's fallback; the
// graphical path needs nothing, it clicks whatever child is named.
enum class NotifyAction {
    None,
    TradeAccept,    // TradeWnd TRDW_Trade_Button
    TradeCancel,    // TradeWnd TRDW_Cancel_Button
    LootAll,        // LootWnd LootAllButton
    LootDone,       // LootWnd DoneButton
    ConfirmYes,     // ConfirmationDialogBox Yes_Button, and OK_Button while it is the only button
    ConfirmNo,      // ConfirmationDialogBox No_Button
    LargeDialogOk,  // LargeDialogWindow LDW_OkButton
    LargeDialogYes, // LargeDialogWindow LDW_YesButton
    LargeDialogNo,  // LargeDialogWindow LDW_NoButton
    TaskDecline,    // TaskSelectWnd DeclineButton (Accept needs the list's selection: the UI's)
    BankAuto,       // BankWnd / BigBankWnd AutoButton
    BankDone,       // BankWnd / BigBankWnd DoneButton
    TributeToggle,  // TributeBenefitWnd ActivateButton
    GuildTributeToggle, // TributeBenefitWnd GuildActivateButton
};
NotifyAction notify_action(std::string_view window, std::string_view child);

} // namespace hailstone::game
