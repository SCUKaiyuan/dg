#include <cassert>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#ifndef HAVE_LLVM
#error "This code needs LLVM enabled"
#endif

#include <llvm/Config/llvm-config.h>

#if (LLVM_VERSION_MAJOR < 3)
#error "Unsupported version of LLVM"
#endif

#include "dg/tools/llvm-slicer-opts.h"
#include "dg/tools/llvm-slicer-utils.h"
#include "dg/tools/llvm-slicer.h"

#if LLVM_VERSION_MAJOR >= 4
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#endif

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR <= 7
#include <llvm/IR/LLVMContext.h>
#endif
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>

#ifdef HAVE_SVF
#include "dg/llvm/PointerAnalysis/SVFPointerAnalysis.h"
#endif
#include "dg/llvm/CallGraph/CallGraph.h"
#include "dg/llvm/PointerAnalysis/DGPointerAnalysis.h"
#include "dg/llvm/PointerAnalysis/PointerAnalysis.h"
#include "dg/util/debug.h"

using namespace dg;

using llvm::errs;

llvm::cl::opt<bool> enable_debug(
        "dbg", llvm::cl::desc("Enable debugging messages (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> usepta("use-pta",
    llvm::cl::desc("Use points analysis to build CG."),
    llvm::cl::init(true), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> lazy("lazy-cg",
    llvm::cl::desc("Use the LazyLLVMCallGraph."),
    llvm::cl::init(true), llvm::cl::cat(SlicingOpts));

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext& context,
                                          const SlicerOptions& options)
{
    llvm::SMDiagnostic SMD;

#if ((LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR <= 5))
    auto _M = llvm::ParseIRFile(options.inputFile, SMD, context);
    auto M = std::unique_ptr<llvm::Module>(_M);
#else
    auto M = llvm::parseIRFile(options.inputFile, SMD, context);
    // _M is unique pointer, we need to get Module *
#endif

    if (!M) {
        SMD.print("llvm-slicer", llvm::errs());
    }

    return M;
}

#ifndef USING_SANITIZERS
void setupStackTraceOnError(int argc, char *argv[]) {
#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR < 9
    llvm::sys::PrintStackTraceOnErrorSignal();
#else
    llvm::sys::PrintStackTraceOnErrorSignal(llvm::StringRef());
#endif
    llvm::PrettyStackTraceProgram X(argc, argv);
}
#else
void setupStackTraceOnError(int, char **) {}
#endif // not USING_SANITIZERS

static void dumpCallGraph(llvmdg::CallGraph &CG) {
    std::cout << "digraph CallGraph {\n";

    for (const auto *f : CG.functions()) {
        for (const auto *c : CG.callees(f)) {
            std::cout << "  \"" << f->getName().str() << "\" -> \""
                      << c->getName().str() << "\"\n";
        }
    }

    std::cout << "}\n";
}

int main(int argc, char *argv[]) {
    setupStackTraceOnError(argc, argv);

    SlicerOptions options = parseSlicerOptions(argc, argv,
                                               /* requireCrit = */ false);

    if (enable_debug) {
        DBG_ENABLE();
    }

    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> M = parseModule(context, options);
    if (!M) {
        llvm::errs() << "Failed parsing '" << options.inputFile << "' file:\n";
        return 1;
    }

    if (!M->getFunction(options.dgOptions.entryFunction)) {
        llvm::errs() << "The entry function not found: "
                     << options.dgOptions.entryFunction << "\n";
        return 1;
    }

    if (usepta) {
        auto& ptaopts = options.dgOptions.PTAOptions;
#ifdef HAVE_SVF
        if (ptaopts.isSVF()) {
            SVFPointerAnalysis PTA(M.get(), ptaopts);
            PTA.run();

            llvmdg::CallGraph CG(M.get(), &PTA, lazy);
            dumpCallGraph(CG);
        } else
#endif // HAVE_SVF
        {
            DGLLVMPointerAnalysis PTA(M.get(), ptaopts);
            PTA.run();

            if (lazy) {
                llvmdg::CallGraph CG(M.get(), &PTA, lazy);
                CG.build();
                dumpCallGraph(CG);
            } else {
                // re-use the call-graph from PTA
                llvmdg::CallGraph CG(PTA.getPTA()->getPG()->getCallGraph());
                dumpCallGraph(CG);
            }
        }
    } else {
        if (!lazy) {
            llvm::errs() << "Can build CG without PTA only with -lazy option\n";
            return 1;
        }

        llvmdg::CallGraph CG(M.get());
        CG.build();
        dumpCallGraph(CG);
    }

    return 0;
}
