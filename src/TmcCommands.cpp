#include "fpp-pch.h"

#include <cmath>
#include <cstdlib>

#include "TmcCommands.h"

#include "AxisWorker.h"
#include "TmcApi.h"
#include "TmcManager.h"

#include "commands/Commands.h"
#include "log.h"

namespace tmc {

static std::vector<Command*> s_commands;

// "all" targets every running stepper; otherwise the argument is a stepper
// name. Names rather than indices: an index means something different after
// the stepper list is edited.
static std::vector<AxisWorker*> resolveTargets(const std::string& which) {
    std::vector<AxisWorker*> out;
    TmcManager& mgr = TmcManager::instance();
    if (which.empty() || which == "all") {
        int n = mgr.axisCount();
        for (int i = 0; i < n; i++) {
            if (AxisWorker* w = mgr.axis(i))
                out.push_back(w);
        }
        return out;
    }
    if (AxisWorker* w = mgr.axisByName(which))
        out.push_back(w);
    return out;
}

// The result reads "<prefix> N stepper(s)<suffix>", with any refusals after a
// semicolon; a target list that refused everywhere is an error.
static std::unique_ptr<Command::Result> submitToAll(const std::string& which,
                                                    const Job& job,
                                                    const std::string& prefix = "Started on",
                                                    const std::string& suffix = "") {
    std::vector<AxisWorker*> targets = resolveTargets(which);
    if (targets.empty())
        return std::make_unique<Command::ErrorResult>("No such stepper: " + which);

    std::string problems;
    int ok = 0;
    for (AxisWorker* w : targets) {
        std::string err;
        if (w->submit(job, err) == 0) {
            if (!problems.empty())
                problems += "; ";
            problems += w->name() + ": " + err;
        } else {
            ok++;
        }
    }
    if (ok == 0)
        return std::make_unique<Command::ErrorResult>(problems);
    std::string text = prefix + " " + std::to_string(ok) + " stepper(s)" + suffix;
    if (!problems.empty())
        text += "; " + problems;
    return std::make_unique<Command::Result>(text);
}

// The stepper-name argument, shared by every command below. The argument
// KEY is "axis": it is stored in saved playlist entries and scheduler
// commands, so it cannot be renamed; only the label can.
static void addAxisArg(std::list<Command::CommandArg>& args) {
    args.push_back(Command::CommandArg("axis", "string", "Stepper")
                       .setDefaultValue("all")
                       .setContentListUrl(std::string(API_PUBLIC_PATH) + "?action=axisNames", true));
}

class TmcRehomeCommand : public Command {
public:
    TmcRehomeCommand() :
        Command("Stepper Motion Rehome",
                "Re-establish a stepper's home reference by driving gently into the "
                "mechanical stop (coarse contact, back off, slow creep back in).") {
        addAxisArg(args);
    }
    std::unique_ptr<Result> run(const std::vector<std::string>& a) override {
        Job job;
        job.kind = Job::Kind::Rehome;
        return submitToAll(a.empty() ? "all" : a[0], job);
    }
};

class TmcGotoCommand : public Command {
public:
    TmcGotoCommand() :
        Command("Stepper Motion Go To Position",
                "Move a stepper to a position given as a percentage of its "
                "commissioned travel.") {
        addAxisArg(args);
        // "string", not "int", so a fractional percentage can be typed: FPP's
        // CommandArg has no float type and an int arg truncates 50.5 to 50,
        // coarse on a short-travel axis. The range is enforced in run().
        args.push_back(CommandArg("position", "string", "Position %")
                           .setDefaultValue("50"));
    }
    std::unique_ptr<Result> run(const std::vector<std::string>& a) override {
        if (a.size() < 2)
            return std::make_unique<ErrorResult>("Position is required");

        // Not atof(), which cannot tell "0" from a word: a mistyped position
        // would silently home the stepper.
        const std::string& raw = a[1];
        char* end = nullptr;
        const double pct = strtod(raw.c_str(), &end);
        if (raw.empty() || end == raw.c_str() || *end != '\0')
            return std::make_unique<ErrorResult>("Position must be a number, 0..100 percent (got '" +
                                                 raw + "')");
        if (!(pct >= 0.0 && pct <= 100.0))
            return std::make_unique<ErrorResult>("Position must be 0..100 percent (got " + raw + ")");

        // The percentage travels as-is: each axis resolves it against its
        // own travel range when the move starts.
        Job job;
        job.kind = Job::Kind::Goto;
        job.target = pct;
        job.targetIsPercent = true;
        // std::to_string on a double gives "50.500000"; echo what was typed.
        return submitToAll(a[0], job, "Moving", " to " + raw + "%");
    }
};

class TmcParkCommand : public Command {
public:
    TmcParkCommand() :
        Command("Stepper Motion Park",
                "Return a stepper to its home position using the stored reference. "
                "This does not re-home: nothing is driven into the stop.") {
        addAxisArg(args);
    }
    std::unique_ptr<Result> run(const std::vector<std::string>& a) override {
        Job job;
        job.kind = Job::Kind::Park;
        return submitToAll(a.empty() ? "all" : a[0], job);
    }
};

class TmcReleaseCommand : public Command {
public:
    TmcReleaseCommand() :
        Command("Stepper Motion Release",
                "De-energise a stepper's coils. The position reference is kept, but "
                "anything that pulls on the shaft (a return spring) will now move "
                "it and invalidate that reference.") {
        addAxisArg(args);
    }
    std::unique_ptr<Result> run(const std::vector<std::string>& a) override {
        Job job;
        job.kind = Job::Kind::Release;
        return submitToAll(a.empty() ? "all" : a[0], job);
    }
};

class TmcStopCommand : public Command {
public:
    TmcStopCommand() :
        Command("Stepper Motion Stop",
                "Immediately halt a stepper wherever it is. Accepted at any time, "
                "including while a sequence is driving it.") {
        addAxisArg(args);
    }
    std::unique_ptr<Result> run(const std::vector<std::string>& a) override {
        std::vector<AxisWorker*> targets = resolveTargets(a.empty() ? "all" : a[0]);
        if (targets.empty())
            return std::make_unique<ErrorResult>("No such stepper");
        for (AxisWorker* w : targets)
            w->abortNow();
        return std::make_unique<Result>("Stopped " + std::to_string(targets.size()) +
                                        " stepper(s)");
    }
};

void registerTmcCommands() {
    if (!s_commands.empty())
        return;
    s_commands.push_back(new TmcRehomeCommand());
    s_commands.push_back(new TmcGotoCommand());
    s_commands.push_back(new TmcParkCommand());
    s_commands.push_back(new TmcReleaseCommand());
    s_commands.push_back(new TmcStopCommand());
    for (Command* c : s_commands)
        CommandManager::INSTANCE.addCategorizedCommand(c, "Stepper Motion");
}

void unregisterTmcCommands() {
    for (Command* c : s_commands) {
        // removeCommand() only unregisters; the object is ours to delete.
        CommandManager::INSTANCE.removeCommand(c);
        delete c;
    }
    s_commands.clear();
}

} // namespace tmc
