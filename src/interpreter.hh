#pragma once

#include <trieste/trieste.h>
#include "lang.hh"
#include "graph.hh"
#include "graphviz.hh"

namespace gitmem
{
    /* For debug printing */
    inline struct Verbose
    {
        bool enabled = false;

        template <typename T>
        const Verbose &operator<<(const T &msg) const
        {
            if (enabled)
            {
                std::cout << msg;
            }
            return *this;
        }

        const Verbose &operator<<(std::ostream &(*manip)(std::ostream &)) const
        {
            if (enabled)
            {
                std::cout << manip;
            }
            return *this;
        }
    } verbose;

    /* A 'Global' is a structure to capture the current synchronising objects
     * representation of a global variable. The structure is the current value,
     * the current commit id for the variable, and the history of commited ids.
     */

    using Commit = size_t;
    using CommitHistory = std::vector<Commit>;

    struct Global
    {
        size_t val;
        std::optional<Commit> commit;
        CommitHistory history;
    };

    using Globals = std::unordered_map<std::string, Global>;

    enum class TerminationStatus
    {
        completed,
        datarace_exception,
        unlock_exception,
        assertion_failure_exception,
        unassigned_variable_read_exception,
    };

    using Locals = std::unordered_map<std::string, size_t>;

    struct ThreadContext
    {
        Locals locals;
        Globals globals;
        std::shared_ptr<graph::Node> tail;
    };

    using ThreadStatus = std::optional<TerminationStatus>;

    struct Thread
    {
        ThreadContext ctx;
        Node block;
        size_t pc = 0;
        ThreadStatus terminated = std::nullopt;

        bool operator==(const Thread &other) const
        {
            // Globals have a history that we don't care about, so we only
            // compare values
            if (ctx.globals.size() != other.ctx.globals.size())
                return false;
            for (const auto &[var, global] : ctx.globals)
            {
                if (!other.ctx.globals.contains(var) ||
                    ctx.globals.at(var).val != other.ctx.globals.at(var).val)
                {
                    return false;
                }
            }
            return ctx.locals == other.ctx.locals &&
                   block == other.block &&
                   pc == other.pc &&
                   terminated == other.terminated;
        }
    };

    using ThreadID = size_t;

    struct Lock
    {
        Globals globals;
        std::optional<ThreadID> owner = std::nullopt;
        std::shared_ptr<graph::Node> last;
    };

    using Threads = std::vector<std::shared_ptr<Thread>>;

    using Locks = std::unordered_map<std::string, struct Lock>;

    template<typename T, typename...Args>
    std::shared_ptr<T> thread_append_node(ThreadContext& ctx, Args&&...args);

    template<>
    std::shared_ptr<graph::Pending> thread_append_node<graph::Pending>(ThreadContext& ctx, std::string&& stmt);

    struct GlobalContext
    {
        Threads threads;
        Locks locks;
        NodeMap<size_t> cache;
        std::shared_ptr<graph::Node> entry_node;
        std::unordered_map<Commit, std::shared_ptr<graph::Node>> commit_map;
        Commit uuid = 0;

        GlobalContext(const Node &ast)
        {
            Node starting_block = ast / File / Block;
            entry_node = std::make_shared<graph::Start>(0);
            ThreadContext starting_ctx = {{}, {}, entry_node};
            auto main_thread = std::make_shared<Thread>(starting_ctx, starting_block);

            this->threads = {main_thread};
            this->locks = {};
            this->cache = {};
        }

        bool operator==(const GlobalContext &other) const
        {
            if (threads.size() != other.threads.size() || locks.size() != other.locks.size())
                return false;

            // Threads may have been spawned in a different order, so we
            // find the thread with the same block in the other context
            for (auto &thread : threads)
            {
                auto it = std::find_if(other.threads.begin(), other.threads.end(),
                                       [&thread](auto &t)
                                       { return t->block == thread->block; });
                if (it == other.threads.end() || !(*thread == **it))
                    return false;
            }

            for (auto &[name, lock] : locks)
            {
                if (!other.locks.contains(name))
                    return false;
                auto &other_lock = other.locks.at(name);
                if (lock.owner != other_lock.owner)
                    return false;
            }
            return true;
        }

        void print_execution_graph(const std::filesystem::path &output_path) const
        {
            // Loop over the threads and add pending nodes to running threads
            // to indicate a threads next step
            for (const auto& t: threads)
            {
                assert(t->ctx.tail);
                if (t->terminated || dynamic_pointer_cast<const graph::Pending>(t->ctx.tail->next))
                    continue;

                Node block = t->block;
                size_t &pc = t->pc;
                Node stmt = block->at(pc);
                thread_append_node<graph::Pending>(t->ctx, std::string(stmt->location().view()));
            }

            graph::GraphvizPrinter gv(output_path);
            gv.visit(entry_node.get());
        }
    };

    enum class ProgressStatus
    {
        progress,
        no_progress
    };

    inline bool operator!(ProgressStatus p) { return p == ProgressStatus::no_progress; }

    inline ProgressStatus operator||(const ProgressStatus &p1, const ProgressStatus &p2)
    {
        return (p1 == ProgressStatus::progress || p2 == ProgressStatus::progress) ? ProgressStatus::progress : ProgressStatus::no_progress;
    }

    inline void operator|=(ProgressStatus &p1, const ProgressStatus &p2) { p1 = (p1 || p2); }

    // Entry functions
    int interpret(const Node, const std::filesystem::path &output_file);
    int interpret_interactive(const Node, const std::filesystem::path &output_file);
    int model_check(const Node, const std::filesystem::path &output_file);

    // Internal functions
    int run_threads(GlobalContext &);

    std::variant<ProgressStatus, TerminationStatus>
    progress_thread(GlobalContext &, const ThreadID, std::shared_ptr<Thread>);
}
