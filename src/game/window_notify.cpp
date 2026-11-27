#include "game/window_notify.h"

#include "game/option_commands.h" // same_no_case
#include "game/slash_commands.h"  // next_cmd_arg, cmd_atol

namespace hailstone::game {
namespace {

constexpr const char* kUsage = "Usage: /notify <window> <child> <leftmouseup|rightmouseup|listselect|tabselect> [value]";

bool all_digits(std::string_view text) {
    if (text.empty()) return false;
    for (char c : text)
        if (c < '0' || c > '9') return false;
    return true;
}

} // namespace

WindowNotify parse_window_notify(std::string_view args) {
    WindowNotify n;
    std::string_view rest = args;
    n.window = next_cmd_arg(rest);
    n.child = next_cmd_arg(rest);
    n.notification = next_cmd_arg(rest);
    const std::string value = next_cmd_arg(rest);
    n.expected = next_cmd_arg(rest);
    if (n.window.empty() || n.child.empty() || n.notification.empty() || !next_cmd_arg(rest).empty()) {
        n.error = kUsage;
        return n;
    }
    const bool click = same_no_case(n.notification, "leftmouseup") || same_no_case(n.notification, "rightmouseup");
    const bool pick = same_no_case(n.notification, "listselect") || same_no_case(n.notification, "tabselect");
    if (!click && !pick) {
        n.error = kUsage;
        return n;
    }
    if (pick) {
        // MQ's row and page are 1-based; the word is the value, and a guard may still follow.
        if (!all_digits(value) || (n.value = cmd_atol(value)) < 1) {
            n.error = kUsage;
            return n;
        }
    } else if (n.expected.empty()) {
        // A click's only trailing word is the guard (the partner or requester it expects).
        n.expected = value;
    } else {
        n.error = kUsage;
        return n;
    }
    return n;
}

NotifyAction notify_action(std::string_view window, std::string_view child) {
    // The screen and child ids are the StockWindows' own (app/lua_ui_items.cpp,
    // lua_ui_player.cpp, lua_ui_progress.cpp), matched as the typed line gives them.
    struct Row {
        std::string_view window;
        std::string_view child;
        NotifyAction action;
    };
    static constexpr Row kRows[] = {
        {"TradeWnd", "TRDW_Trade_Button", NotifyAction::TradeAccept},
        {"TradeWnd", "TRDW_Cancel_Button", NotifyAction::TradeCancel},
        {"LootWnd", "LootAllButton", NotifyAction::LootAll},
        {"LootWnd", "DoneButton", NotifyAction::LootDone},
        {"ConfirmationDialogBox", "Yes_Button", NotifyAction::ConfirmYes},
        {"ConfirmationDialogBox", "OK_Button", NotifyAction::ConfirmYes},
        {"ConfirmationDialogBox", "No_Button", NotifyAction::ConfirmNo},
        {"LargeDialogWindow", "LDW_OkButton", NotifyAction::LargeDialogOk},
        {"LargeDialogWindow", "LDW_YesButton", NotifyAction::LargeDialogYes},
        {"LargeDialogWindow", "LDW_NoButton", NotifyAction::LargeDialogNo},
        {"TaskSelectWnd", "DeclineButton", NotifyAction::TaskDecline},
        {"BankWnd", "AutoButton", NotifyAction::BankAuto},
        {"BankWnd", "DoneButton", NotifyAction::BankDone},
        {"BigBankWnd", "AutoButton", NotifyAction::BankAuto},
        {"BigBankWnd", "DoneButton", NotifyAction::BankDone},
        {"TributeBenefitWnd", "ActivateButton", NotifyAction::TributeToggle},
        {"TributeBenefitWnd", "GuildActivateButton", NotifyAction::GuildTributeToggle},
    };
    for (const Row& row : kRows)
        if (same_no_case(window, row.window) && same_no_case(child, row.child)) return row.action;
    return NotifyAction::None;
}

} // namespace hailstone::game
