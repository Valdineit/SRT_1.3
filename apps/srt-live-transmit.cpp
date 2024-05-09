#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#define REQUIRE_CXX11 1

// Inclusão de bibliotecas padrão do C++

#include <cctype>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <memory>
#include <algorithm>

// Inclusão de bibliotecas de sistema

#include <iterator>
#include <stdexcept>
#include <cstring>
#include <csignal>
#include <chrono>
#include <thread>
#include <list>

// Inclusão de bibliotecas personalizadas

#include "srt_compat.h"
#include "apputil.hpp"  // CreateAddr
#include "uriparser.hpp"  // UriParser
#include "socketoptions.hpp"
#include "logsupport.hpp"
#include "transmitmedia.hpp"
#include "verbose.hpp"

// Inclusão das bibliotecas SRT

#include <srt.h>
#include <logging.h>
#include <srt_udp.h> // Adicionado para o uso do buffer de recebimento UDP

// Espaço de nomes utilizado
using namespace std;

// Definição de exceções personalizadas
struct ForcedExit: public std::runtime_error
{
    ForcedExit(const std::string& arg):
        std::runtime_error(arg)
    {
    }
};

struct AlarmExit: public std::runtime_error
{
    AlarmExit(const std::string& arg):
        std::runtime_error(arg)
    {
    }
};

// Variáveis atômicas para controle de interrupções
srt::sync::atomic<bool> int_state;
srt::sync::atomic<bool> timer_state;

// Função de tratamento de interrupção por sinal SIGALRM
void OnINT_ForceExit(int)
{
    Verb() << "\n-------- REQUESTED INTERRUPT!\n";
    int_state = true;
}

void OnAlarm_Interrupt(int)
{
    Verb() << "\n---------- INTERRUPT ON TIMEOUT!\n";

    int_state = false; // JIC
    timer_state = true;

    if ((false))
    {
        throw AlarmExit("Watchdog bites hangup");
    }
}

// Função de tratamento de log para o SRT
extern "C" void TestLogHandler(void* opaque, int level, const char* file, int line, const char* area, const char* message);

// Estrutura de configuração para a transmissão ao vivo
struct LiveTransmitConfig
{
    // Configurações de timeout, tamanho de chunk, etc.
    int timeout = 0;
    int timeout_mode = 0;
    int chunk_size = -1;
    bool quiet = false;
    srt_logging::LogLevel::type loglevel = srt_logging::LogLevel::error;
    set<srt_logging::LogFA> logfas;
    bool log_internal;
    string logfile;
    int bw_report = 0;
    bool srctime = false;
    size_t buffering = 0; // Alterado para 0 para que seja definido posteriormente
    int stats_report = 0;
    string stats_out;
    SrtStatsPrintFormat stats_pf = SRTSTATS_PROFMAT_2COLS;
    bool auto_reconnect = true;
    bool full_stats = false;

    string source;
    string target;
};

// Função para imprimir ajuda sobre as opções de linha de comando
void PrintOptionHelp(const OptionName& opt_names, const string &value, const string &desc)
{
    cerr << "\t";
    int i = 0;
    for (auto opt : opt_names.names)
    {
        if (i++) cerr << ", ";
        cerr << "-" << opt;
    }

    if (!value.empty())
        cerr << ":"  << value;
    cerr << "\t- " << desc << "\n";
}

// Função para analisar e processar os argumentos da linha de comando
int parse_args(LiveTransmitConfig &cfg, int argc, char** argv)
{
    // Definição das opções de linha de comando
    const OptionName
        o_timeout       = { "t", "to", "timeout" },
        o_timeout_mode  = { "tm", "timeout-mode" },
        o_autorecon     = { "a", "auto", "autoreconnect" },
        o_chunk         = { "c", "chunk" },
        o_bwreport      = { "r", "bwreport", "report", "bandwidth-report", "bitrate-report" },
        o_srctime       = {"st", "srctime", "sourcetime"},
        o_buffering     = {"buffering"},
        o_statsrep      = { "s", "stats", "stats-report-frequency" },
        o_statsout      = { "statsout" },
        o_statspf       = { "pf", "statspf" },
        o_statsfull     = { "f", "fullstats" },
        o_loglevel      = { "ll", "loglevel" },
        o_logfa         = { "lfa", "logfa" },
        o_log_internal  = { "loginternal"},
        o_logfile       = { "logfile" },
        o_quiet         = { "q", "quiet" },
        o_verbose       = { "v", "verbose" },
        o_help          = { "h", "help" },
        o_version       = { "version" };

    // Vetor de esquemas de opções
    const vector<OptionScheme> optargs = {
        { o_timeout,      OptionScheme::ARG_ONE },
        { o_timeout_mode, OptionScheme::ARG_ONE },
        { o_autorecon,    OptionScheme::ARG_ONE },
        { o_chunk,        OptionScheme::ARG_ONE },
        { o_bwreport,     OptionScheme::ARG_ONE },
        { o_srctime,      OptionScheme::ARG_ONE },
        { o_buffering,    OptionScheme::ARG_ONE },
        { o_statsrep,     OptionScheme::ARG_ONE },
        { o_statsout,     OptionScheme::ARG_ONE },
        { o_statspf,      OptionScheme::ARG_ONE },
        { o_statsfull,    OptionScheme::ARG_NONE },
        { o_loglevel,     OptionScheme::ARG_ONE },
        { o_logfa,        OptionScheme::ARG_ONE },
        { o_log_internal, OptionScheme::ARG_NONE },
        { o_logfile,      OptionScheme::ARG_ONE },
        { o_quiet,        OptionScheme::ARG_NONE },
        { o_verbose,      OptionScheme::ARG_NONE },
        { o_help,         OptionScheme::ARG_NONE },
        { o_version,      OptionScheme::ARG_NONE }
    };

    // Processamento das opções e argumentos da linha de comando
    // Retorno indicando sucesso, falha ou necessidade de exibição da ajuda
    string optstr;
    int ret = 0;
    try
    {
        optstr = GetOpts(argc, argv, optargs);

        for (auto arg : optargs)
        {
            auto opt = arg.option;
            if (optstr.find(opt.names[0]) == string::npos)
                continue;

            switch (opt.names[0][0])
            {
            case 't':
                cfg.timeout = stoi(arg.arg);
                break;
            case 'a':
                cfg.auto_reconnect = arg.arg.empty() || stoi(arg.arg);
                break;
            case 'c':
                cfg.chunk_size = stoi(arg.arg);
                break;
            case 'r':
                cfg.bw_report = stoi(arg.arg);
                break;
            case 's':
                cfg.stats_report = stoi(arg.arg);
                break;
            case 'p':
                cfg.stats_pf = static_cast<SrtStatsPrintFormat>(stoi(arg.arg));
                break;
            case 'f':
                cfg.full_stats = true;
                break;
            case 'l':
                if (opt.names[0][1] == 'l')
                    cfg.loglevel = srt_logging::StringToLevel(arg.arg);
                else
                    cfg.log_internal = true;
                break;
            case 'q':
                cfg.quiet = true;
                break;
            case 'h':
                return -1;
            case 'v':
                // verbosity can be increased by repeating '-v'
                if (!arg.arg.empty())
                    for (int i = 0; i < stoi(arg.arg); ++i)
                        Verb() << "Verbosity increased.\n";
                break;
            default:
                ret = 1;
            }
        }

        // Special cases, as not directly associated with option letter
        if (cfg.chunk_size < 0)  // not set or error
        {
            cerr << "Chunk size is mandatory!\n";
            return -1;
        }
        if (optstr.find("-lfa") != string::npos)
        {
            cfg.logfas = srt_logging::StringToLogFA(arg.arg);
        }
        if (optstr.find("-buffering") != string::npos)
        {
            cfg.buffering = stoi(arg.arg);
        }
        if (optstr.find("-st") != string::npos)
        {
            cfg.srctime = true;
        }
        if (optstr.find("-logfile") != string::npos)
        {
            cfg.logfile = arg.arg;
        }

        for (auto& arg : optargs)
        {
            auto opt = arg.option;
            if (optstr.find(opt.names[0]) == string::npos)
                continue;

            switch (opt.names[0][0])
            {
            case 'h':
                cerr << "Help\n";
                return -1;
            case 'v':
                break;
            }
        }

        auto args = GetArgs(argc, argv);
        if (args.size() != 2)
            throw std::invalid_argument("Expecting 2 parameters");
        cfg.source = args[0];
        cfg.target = args[1];
    }
    catch (std::invalid_argument& ia)
    {
        cerr << "Invalid argument: " << ia.what() << "\n";
        return -1;
    }
    catch (std::exception& e)
    {
        cerr << "Error: " << e.what() << "\n";
        return -1;
    }

    return ret;
}

// Função principal do programa
int main(int argc, char** argv)
{
    LiveTransmitConfig cfg;

    try
    {
        if (parse_args(cfg, argc, argv) < 0)
        {
            cerr << "Usage: " << argv[0] << " [options] <source> <target>\n\n"
                 << "Options:\n";

            PrintOptionHelp(OptionName{ "timeout" }, "milliseconds", "Set overall connection timeout");
            PrintOptionHelp(OptionName{ "timeout-mode" }, "[0|1|2|3] (0=never, 1=on read, 2=on write, 3=on read&write)", "Set timeout handling mode");
            PrintOptionHelp(OptionName{ "chunk" }, "bytes", "Set maximum chunk size");
            PrintOptionHelp(OptionName{ "bwreport" }, "milliseconds", "Set bandwidth report interval");
            PrintOptionHelp(OptionName{ "srctime" }, "", "Use source time for scheduling");
            PrintOptionHelp(OptionName{ "buffering" }, "milliseconds", "Set buffering (UDP only)");
            PrintOptionHelp(OptionName{ "stats" }, "milliseconds", "Set statistics print interval");
            PrintOptionHelp(OptionName{ "statsout" }, "filename", "Write statistics to file");
            PrintOptionHelp(OptionName{ "statspf" }, "[1|2] (1=vertical, 2=horizontal)", "Set statistics print format");
            PrintOptionHelp(OptionName{ "fullstats" }, "", "Use full stats");
            PrintOptionHelp(OptionName{ "loglevel" }, "[trace|debug|info|warn|error|fatal]", "Set log level");
            PrintOptionHelp(OptionName{ "logfa" }, "[+|-]<fa>,<fa>,...", "Set log facilities");
            PrintOptionHelp(OptionName{ "loginternal" }, "", "Log SRT internal messages");
            PrintOptionHelp(OptionName{ "logfile" }, "filename", "Write log to file");
            PrintOptionHelp(OptionName{ "quiet" }, "", "Suppress all except errors");
            PrintOptionHelp(OptionName{ "verbose" }, "", "Verbose output");

            return -1;
        }

        Verb() << "Starting...\n";

        srt_startup();

        int_state = false;
        timer_state = false;

        signal(SIGINT, OnINT_ForceExit);
        signal(SIGTERM, OnINT_ForceExit);
        signal(SIGALRM, OnAlarm_Interrupt);

        // Configurando buffer UDP ajustável
        if (cfg.buffering > 0)
        {
            if (srt_setsockflag(NULL, SRTO_RCVBUF, &cfg.buffering, sizeof(cfg.buffering)) == SRT_ERROR)
            {
                throw std::runtime_error("Failed to set UDP receive buffer size");
            }
        }

        // Código principal de transmissão aqui
         if (print_version)
    {
        PrintLibVersion();
        return 2;
    }

    cfg.timeout      = Option<OutNumber>(params, o_timeout);
    cfg.timeout_mode = Option<OutNumber>(params, o_timeout_mode);
    cfg.chunk_size   = Option<OutNumber>(params, "-1", o_chunk);
    cfg.srctime      = Option<OutBool>(params, cfg.srctime, o_srctime);
    const int buffering = Option<OutNumber>(params, "10", o_buffering);
    if (buffering <= 0)
    {
        cerr << "ERROR: Buffering value should be positive. Value provided: " << buffering << "." << endl;
        return 1;
    }
    else
    {
        cfg.buffering = (size_t) buffering;
    }
    cfg.bw_report    = Option<OutNumber>(params, o_bwreport);
    cfg.stats_report = Option<OutNumber>(params, o_statsrep);
    cfg.stats_out    = Option<OutString>(params, o_statsout);
    const string pf  = Option<OutString>(params, "default", o_statspf);
    string pfext;
    cfg.stats_pf     = ParsePrintFormat(pf, (pfext));
    if (cfg.stats_pf == SRTSTATS_PROFMAT_INVALID)
    {
        cfg.stats_pf = SRTSTATS_PROFMAT_2COLS;
        cerr << "ERROR: Unsupported print format: " << pf << " -- fallback to default" << endl;
        return 1;
    }

    cfg.full_stats   = OptionPresent(params, o_statsfull);
    cfg.loglevel     = SrtParseLogLevel(Option<OutString>(params, "warn", o_loglevel));
    cfg.logfas       = SrtParseLogFA(Option<OutString>(params, "", o_logfa));
    cfg.log_internal = OptionPresent(params, o_log_internal);
    cfg.logfile      = Option<OutString>(params, o_logfile);
    cfg.quiet        = OptionPresent(params, o_quiet);
    
    if (OptionPresent(params, o_verbose))
        Verbose::on = !cfg.quiet;

    cfg.auto_reconnect = Option<OutBool>(params, true, o_autorecon);

    cfg.source = params[""].at(0);
    cfg.target = params[""].at(1);

    return 0;
}


// Função principal do programa
int main(int argc, char** argv)
{
    srt_startup();
    
    if (!SysInitializeNetwork())
        throw std::runtime_error("Can't initialize network!");

    // Symmetrically, this does a cleanup; put into a local destructor to ensure that
    // it's called regardless of how this function returns.
    struct NetworkCleanup
    {
        ~NetworkCleanup()
        {
            srt_cleanup();
            SysCleanupNetwork();
        }
    } cleanupobj;


    LiveTransmitConfig cfg;
    const int parse_ret = parse_args(cfg, argc, argv);
    if (parse_ret != 0)
        return parse_ret == 1 ? EXIT_FAILURE : 0;

    //
    // Set global config variables
    //
    if (cfg.chunk_size > 0)
        transmit_chunk_size = cfg.chunk_size;
    transmit_stats_writer = SrtStatsWriterFactory(cfg.stats_pf);
    transmit_bw_report = cfg.bw_report;
    transmit_stats_report = cfg.stats_report;
    transmit_total_stats = cfg.full_stats;

    //
    // Set SRT log levels and functional areas
    //
    srt_setloglevel(cfg.loglevel);
    if (!cfg.logfas.empty())
    {
        srt_resetlogfa(nullptr, 0);
        for (set<srt_logging::LogFA>::iterator i = cfg.logfas.begin(); i != cfg.logfas.end(); ++i)
            srt_addlogfa(*i);
    }

    //
    // SRT log handler
    //
    std::ofstream logfile_stream; // leave unused if not set
    char NAME[] = "SRTLIB";
    if (cfg.log_internal)
    {
        srt_setlogflags(0
            | SRT_LOGF_DISABLE_TIME
            | SRT_LOGF_DISABLE_SEVERITY
            | SRT_LOGF_DISABLE_THREADNAME
            | SRT_LOGF_DISABLE_EOL
        );
        srt_setloghandler(NAME, TestLogHandler);
    }
    else if (!cfg.logfile.empty())
    {
        logfile_stream.open(cfg.logfile.c_str());
        if (!logfile_stream)
        {
            cerr << "ERROR: Can't open '" << cfg.logfile.c_str() << "' for writing - fallback to cerr\n";
        }
        else
        {
            srt::setlogstream(logfile_stream);
        }
    }


    //
    // SRT stats output
    //
    std::ofstream logfile_stats; // leave unused if not set
    if (cfg.stats_out != "")
    {
        logfile_stats.open(cfg.stats_out.c_str());
        if (!logfile_stats)
        {
            cerr << "ERROR: Can't open '" << cfg.stats_out << "' for writing stats. Fallback to stdout.\n";
            logfile_stats.close();
        }
    }
    else if (cfg.bw_report != 0 || cfg.stats_report != 0)
    {
        g_stats_are_printed_to_stdout = true;
    }

    ostream &out_stats = logfile_stats.is_open() ? logfile_stats : cout;

#ifdef _WIN32

    if (cfg.timeout != 0)
    {
        cerr << "ERROR: The -timeout option (-t) is not implemented on Windows\n";
        return EXIT_FAILURE;
    }

#else
    if (cfg.timeout > 0)
    {
        signal(SIGALRM, OnAlarm_Interrupt);
        if (!cfg.quiet)
            cerr << "TIMEOUT: will interrupt after " << cfg.timeout << "s\n";
        alarm(cfg.timeout);
    }
#endif
    signal(SIGINT, OnINT_ForceExit);
    signal(SIGTERM, OnINT_ForceExit);


    if (!cfg.quiet)
    {
        cerr << "Media path: '"
            << cfg.source
            << "' --> '"
            << cfg.target
            << "'\n";
    }

    unique_ptr<Source> src;
    bool srcConnected = false;
    unique_ptr<Target> tar;
    bool tarConnected = false;

    int pollid = srt_epoll_create();
    if (pollid < 0)
    {
        cerr << "Can't initialize epoll";
        return 1;
    }

    size_t receivedBytes = 0;
    size_t wroteBytes = 0;
    size_t lostBytes = 0;
    size_t lastReportedtLostBytes = 0;
    std::time_t writeErrorLogTimer(std::time(nullptr));

    try {
        // Now loop until broken
        while (!int_state && !timer_state)
        {
            if (!src.get())
            {
                src = Source::Create(cfg.source);
                if (!src.get())
                {
                    cerr << "Unsupported source type" << endl;
                    return 1;
                }
                int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
                switch (src->uri.type())
                {
                case UriParser::SRT:
                    if (srt_epoll_add_usock(pollid,
                        src->GetSRTSocket(), &events))
                    {
                        cerr << "Failed to add SRT source to poll, "
                            << src->GetSRTSocket() << endl;
                        return 1;
                    }
                    break;
                case UriParser::UDP:
                case UriParser::RTP:
                    if (srt_epoll_add_ssock(pollid,
                        src->GetSysSocket(), &events))
                    {
                        cerr << "Failed to add " << src->uri.proto()
                            << " source to poll, " << src->GetSysSocket()
                            << endl;
                        return 1;
                    }
                    break;
                case UriParser::FILE:
                    if (srt_epoll_add_ssock(pollid,
                        src->GetSysSocket(), &events))
                    {
                        cerr << "Failed to add FILE source to poll, "
                            << src->GetSysSocket() << endl;
                        return 1;
                    }
                    break;
                default:
                    break;
                }

                receivedBytes = 0;
            }

            if (!tar.get())
            {
                tar = Target::Create(cfg.target);
                if (!tar.get())
                {
                    cerr << "Unsupported target type" << endl;
                    return 1;
                }

                // IN because we care for state transitions only
                // OUT - to check the connection state changes
                int events = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
                switch(tar->uri.type())
                {
                case UriParser::SRT:
                    if (srt_epoll_add_usock(pollid,
                        tar->GetSRTSocket(), &events))
                    {
                        cerr << "Failed to add SRT destination to poll, "
                            << tar->GetSRTSocket() << endl;
                        return 1;
                    }
                    break;
                default:
                    break;
                }

                wroteBytes = 0;
                lostBytes = 0;
                lastReportedtLostBytes = 0;
            }

            int srtrfdslen = 2;
            int srtwfdslen = 2;
            SRTSOCKET srtrwfds[4] = {SRT_INVALID_SOCK, SRT_INVALID_SOCK , SRT_INVALID_SOCK , SRT_INVALID_SOCK };
            int sysrfdslen = 2;
            SYSSOCKET sysrfds[2];
            if (srt_epoll_wait(pollid,
                &srtrwfds[0], &srtrfdslen, &srtrwfds[2], &srtwfdslen,
                100,
                &sysrfds[0], &sysrfdslen, 0, 0) >= 0)
            {
                bool doabort = false;
                for (size_t i = 0; i < sizeof(srtrwfds) / sizeof(SRTSOCKET); i++)
                {
                    SRTSOCKET s = srtrwfds[i];
                    if (s == SRT_INVALID_SOCK)
                        continue;

                    // Remove duplicated sockets
                    for (size_t j = i + 1; j < sizeof(srtrwfds) / sizeof(SRTSOCKET); j++)
                    {
                        const SRTSOCKET next_s = srtrwfds[j];
                        if (next_s == s)
                            srtrwfds[j] = SRT_INVALID_SOCK;
                    }

                    bool issource = false;
                    if (src && src->GetSRTSocket() == s)
                    {
                        issource = true;
                    }
                    else if (tar && tar->GetSRTSocket() != s)
                    {
                        continue;
                    }

                    const char * dirstring = (issource) ? "source" : "target";

                    SRT_SOCKSTATUS status = srt_getsockstate(s);
                    switch (status)
                    {
                    case SRTS_LISTENING:
                    {
                        const bool res = (issource) ?
                            src->AcceptNewClient() : tar->AcceptNewClient();
                        if (!res)
                        {
                            cerr << "Failed to accept SRT connection"
                                << endl;
                            doabort = true;
                            break;
                        }

                        srt_epoll_remove_usock(pollid, s);

                        SRTSOCKET ns = (issource) ?
                            src->GetSRTSocket() : tar->GetSRTSocket();
                        int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
                        if (srt_epoll_add_usock(pollid, ns, &events))
                        {
                            cerr << "Failed to add SRT client to poll, "
                                << ns << endl;
                            doabort = true;
                        }
                        else
                        {
                            if (!cfg.quiet)
                            {
                                cerr << "Accepted SRT "
                                    << dirstring
                                    <<  " connection"
                                    << endl;
                            }
#ifndef _WIN32
                            if (cfg.timeout_mode == 1 && cfg.timeout > 0)
                            {
                                if (!cfg.quiet)
                                    cerr << "TIMEOUT: cancel\n";
                                alarm(0);
                            }
#endif
                            if (issource)
                                srcConnected = true;
                            else
                                tarConnected = true;
                        }
                    }
                    break;
                    case SRTS_BROKEN:
                    case SRTS_NONEXIST:
                    case SRTS_CLOSED:
                    {
                        if (issource)
                        {
                            if (srcConnected)
                            {
                                if (!cfg.quiet)
                                {
                                    cerr << "SRT source disconnected"
                                        << endl;
                                }
                                srcConnected = false;
                            }
                        }
                        else if (tarConnected)
                        {
                            if (!cfg.quiet)
                                cerr << "SRT target disconnected" << endl;
                            tarConnected = false;
                        }

                        if(!cfg.auto_reconnect)
                        {
                            doabort = true;
                        }
                        else
                        {
                            // force re-connection
                            srt_epoll_remove_usock(pollid, s);
                            if (issource)
                                src.reset();
                            else
                                tar.reset();

#ifndef _WIN32
                            if (cfg.timeout_mode == 1 && cfg.timeout > 0)
                            {
                                if (!cfg.quiet)
                                    cerr << "TIMEOUT: will interrupt after " << cfg.timeout << "s\n";
                                alarm(cfg.timeout);
                            }
#endif
                        }
                    }
                    break;
                    case SRTS_CONNECTED:
                    {
                        if (issource)
                        {
                            if (!srcConnected)
                            {
                                if (!cfg.quiet)
                                    cerr << "SRT source connected" << endl;
                                srcConnected = true;
                            }
                        }
                        else if (!tarConnected)
                        {
                            if (!cfg.quiet)
                                cerr << "SRT target connected" << endl;
                            tarConnected = true;
                            if (tar->uri.type() == UriParser::SRT)
                            {
                                const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
                                // Disable OUT event polling when connected
                                if (srt_epoll_update_usock(pollid,
                                    tar->GetSRTSocket(), &events))
                                {
                                    cerr << "Failed to add SRT destination to poll, "
                                        << tar->GetSRTSocket() << endl;
                                    return 1;
                                }
                            }

#ifndef _WIN32
                            if (cfg.timeout_mode == 1 && cfg.timeout > 0)
                            {
                                if (!cfg.quiet)
                                    cerr << "TIMEOUT: cancel\n";
                                alarm(0);
                            }
#endif
                        }
                    }

                    default:
                    {
                        // No-Op
                    }
                    break;
                    }
                }

                if (doabort)
                {
                    break;
                }

                // read a few chunks at a time in attempt to deplete
                // read buffers as much as possible on each read event
                // note that this implies live streams and does not
                // work for cached/file sources
                std::list<std::shared_ptr<MediaPacket>> dataqueue;
                if (src.get() && src->IsOpen() && (srtrfdslen || sysrfdslen))
                {
                    while (dataqueue.size() < cfg.buffering)
                    {
                        std::shared_ptr<MediaPacket> pkt(new MediaPacket(transmit_chunk_size));
                        const int res = src->Read(transmit_chunk_size, *pkt, out_stats);

                        if (res == SRT_ERROR && src->uri.type() == UriParser::SRT)
                        {
                            if (srt_getlasterror(NULL) == SRT_EASYNCRCV)
                                break;

                            throw std::runtime_error(
                                string("error: recvmsg: ") + string(srt_getlasterror_str())
                            );
                        }

                        if (res == 0 || pkt->payload.empty())
                        {
                            break;
                        }

                        dataqueue.push_back(pkt);
                        receivedBytes += pkt->payload.size();
                    }
                }

                // if there is no target, let the received data be lost
                while (!dataqueue.empty())
                {
                    std::shared_ptr<MediaPacket> pkt = dataqueue.front();
                    if (!tar.get() || !tar->IsOpen())
                    {
                        lostBytes += pkt->payload.size();
                    }
                    else if (!tar->Write(pkt->payload.data(), pkt->payload.size(), cfg.srctime ? pkt->time : 0, out_stats))
                    {
                        lostBytes += pkt->payload.size();
                    }
                    else
                    {
                        wroteBytes += pkt->payload.size();
                    }

                    dataqueue.pop_front();
                }

                if (!cfg.quiet && (lastReportedtLostBytes != lostBytes))
                {
                    std::time_t now(std::time(nullptr));
                    if (std::difftime(now, writeErrorLogTimer) >= 5.0)
                    {
                        cerr << lostBytes << " bytes lost, "
                            << wroteBytes << " bytes sent, "
                            << receivedBytes << " bytes received"
                            << endl;
                        writeErrorLogTimer = now;
                        lastReportedtLostBytes = lostBytes;
                    }
                }
            }
        }
    }
    catch (std::exception& x)
    {
        cerr << "ERROR: " << x.what() << endl;
        return 255;
    }

    return 0;
}

// Class utilities
// Função de tratamento de log para o SRT

void TestLogHandler(void* opaque, int level, const char* file, int line, const char* area, const char* message)
{
    char prefix[100] = "";
    if ( opaque ) {
#ifdef _MSC_VER
        strncpy_s(prefix, sizeof(prefix), (char*)opaque, _TRUNCATE);
#else
        strncpy(prefix, (char*)opaque, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';
#endif
    }
    time_t now;
    time(&now);
    char buf[1024];
    struct tm local = SysLocalTime(now);
    size_t pos = strftime(buf, 1024, "[%c ", &local);

#ifdef _MSC_VER
    // That's something weird that happens on Microsoft Visual Studio 2013
    // Trying to keep portability, while every version of MSVS is a different plaform.
    // On MSVS 2015 there's already a standard-compliant snprintf, whereas _snprintf
    // is available on backward compatibility and it doesn't work exactly the same way.
#define snprintf _snprintf
#endif
    snprintf(buf+pos, 1024-pos, "%s:%d(%s)]{%d} %s", file, line, area, level, message);

    cerr << buf << endl;
}

    // O código de transmissão acaba aqui.

        Verb() << "Exiting...\n";

        srt_cleanup();
    }
    catch (ForcedExit&)
    {
        Verb() << "Forced exit\n";
    }
    catch (AlarmExit&)
    {
        Verb() << "Alarm exit\n";
    }
    catch (std::exception& e)
    {
        cerr << "Exception: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
