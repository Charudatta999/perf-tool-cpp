#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sstream>
#include <thread>
#include <dirent.h>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <map>
// #include <hwloc.h>
using namespace std;

pid_t findProcess(const std::string &processName)
{
    DIR *dir = opendir("/proc");
    if (dir == nullptr)
    {
        perror("opendir failed");
        return -1;
    }

    dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_type != DT_DIR)
        {
            continue;
        }

        pid_t pid = strtol(entry->d_name, nullptr, 10);
        if (pid == 0)
        {
            continue;
        }

        std::string cmd_path = "/proc/" + std::to_string(pid) + "/cmdline";
        std::ifstream cmd_file(cmd_path);
        if (cmd_file.is_open())
        {
            std::string cmd;
            std::getline(cmd_file, cmd, '\0');
            size_t pos = cmd.find_last_of('/');
            std::string cmd_name = (pos != std::string::npos) ? cmd.substr(pos + 1) : cmd;

            if (cmd_name == processName)
            {
                closedir(dir);
                return pid;
            }
        }
    }

    closedir(dir);
    return -1;
}

double get_uptime_in_seconds()
{
    ifstream uptime_file("/proc/uptime");
    if (uptime_file.is_open())
    {
        double uptime_seconds;
        uptime_file >> uptime_seconds;
        uptime_file.close();
        return uptime_seconds;
    }
    return -1.0;
}

double getCPUUsage(pid_t pid)
{
    string stat_path = "/proc/" + to_string(pid) + "/stat";
    ifstream stat_file(stat_path);
    if (stat_file.is_open())
    {
        string line;
        getline(stat_file, line);
        stat_file.close();
        istringstream iss(line);
        vector<string> tokens;
        copy(istream_iterator<string>(iss), istream_iterator<string>(), back_inserter(tokens));
        double utime_ticks = stod(tokens[13]);
        double stime_ticks = stod(tokens[14]);
        double cutime_ticks = stod(tokens[15]);
        double cstime_ticks = stod(tokens[16]);
        double total_time_ticks = utime_ticks + stime_ticks + cutime_ticks + cstime_ticks;
        double system_uptime_ticks = sysconf(_SC_CLK_TCK) * get_uptime_in_seconds();
        double cpu_usage_percent = (total_time_ticks / system_uptime_ticks) * 100;
        return cpu_usage_percent;
    }
    return -1.0;
}

vector<double> getMemUsage(pid_t pid)
{
    std::string stat_path = "/proc/" + to_string(pid) + "/stat";
    ifstream stat_file(stat_path);
    if (stat_file.is_open())
    {
        std::string line;
        getline(stat_file, line);
        istringstream iss(line);
        vector<std::string> tokens;
        copy(istream_iterator<std::string>(iss), istream_iterator<std::string>(), back_inserter(tokens));
        long rss_pages = stol(tokens[23]);
        double rss_mem_in_bytes = rss_pages * sysconf(_SC_PAGESIZE);

        std::string statm_path = "/proc/" + to_string(pid) + "/statm";
        ifstream statm_file(statm_path);
        if (statm_file.is_open())
        {
            std::string statm_value;
            statm_file >> statm_value;
            long mem_in_pages = stol(statm_value);
            double mem_in_bytes = mem_in_pages * sysconf(_SC_PAGESIZE);
            double virt_mem_in_bytes = mem_in_bytes - rss_mem_in_bytes;
            double total_mem_in_bytes = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
            double mem_usage_percent = (mem_in_bytes / total_mem_in_bytes) * 100;

            vector<double> mem_usage = {rss_mem_in_bytes, virt_mem_in_bytes, total_mem_in_bytes, mem_usage_percent};
            return mem_usage;
        }
    }
    return {-1, -1, -1, -1};
}

double getMemoryPercentage(const vector<double> &memUsage)
{
    return (memUsage[0] / memUsage[2]) * 100;
}
void writeOutputLine(ofstream &file, time_t timestamp, pid_t pid, double value, bool csvFormat = true)
{
    std::string timestamp_str = ctime(&timestamp);
    if (timestamp_str.back() == '\n')
    {
        timestamp_str.pop_back();
    }
    if (csvFormat)
    {
        file << timestamp_str << "," << pid << "," << value << std::endl;
    }
    else
    {

        file << "{\"Timestamp\": \"" << timestamp_str << "\", "
             << "\"PID\": " << pid << ", "
             << "\"Value\": " << value << "}" << std::endl;
    }
    file.flush();
}

void writeOutputLine(ofstream &file, time_t timestamp, pid_t pid, const vector<double> &memUsage, bool csvFormat = true)
{
    string timestamp_str = ctime(&timestamp);
    if (timestamp_str.back() == '\n')
    {
        timestamp_str.pop_back();
    }
    if (csvFormat)
    {
        file << timestamp_str << "," << pid << "," << memUsage[0] << "," << memUsage[1] << "," << memUsage[2] << ","
             << memUsage[3] << "," << memUsage[4] << "," << getMemoryPercentage(memUsage) << std::endl;
    }
    else
    {

        file << "{\"Timestamp\": \"" << timestamp_str << "\", "
             << "\"PID\": " << pid << ", "
             << "\"RSS Memory\": " << memUsage[0] << ", "
             << "\"Virtual Memory\": " << memUsage[1] << ", "
             << "\"Total Memory\": " << memUsage[2] << ", "
             << "\"PSS Memory\": " << memUsage[3] << ", "
             << "\"USS Memory\": " << memUsage[4] << ", "
             << "\"Memory Percentage\": " << getMemoryPercentage(memUsage) << "}" << std::endl;
    }
    file.flush();
}

void monitorProcess(std::string processName, int cpuInterval, int memInterval, std::string outputdir, bool csvFormat)
{
    std::string cpuFilename = outputdir + "/" + processName + "_cpu.txt";
    std::string memFilename = outputdir + "/" + processName + "_mem.txt";
    std::string restarts_file = outputdir + "/" + processName + "_restarts.txt";
    ofstream cpu_file(cpuFilename);
    ofstream mem_file(memFilename);
    ofstream restarts(restarts_file);
    cpu_file << "Timestamp,PID,Value" << std::endl;
    mem_file << "Timestamp,PID,RSS Memory,Virtual Memory,Total Memory,PSS Memory,USS Memory,Memory Percentage" << std::endl;
    restarts << "Timestamp,PID,Reason" << std::endl;
    mem_file.flush();
    cpu_file.flush();
    restarts.flush();
    pid_t pid = findProcess(processName);
    time_t last_modified = time(NULL);
    bool process_found = (pid != -1);
    if (!process_found)
    {
        cerr << "Error: Process " << processName << " not found" << std::endl;
    }

    int cpuCounter = 0;
    int memCounter = 0;

    while (true)
    {
        // Check if process has restarted or PID has changed
        pid_t new_pid = findProcess(processName);
        if (new_pid != pid)
        {
            std::cout << "PID changed for process " << processName << "Pid is :" << new_pid << std::endl;
            pid = new_pid;
            last_modified = time(NULL);
            char time_str[100];
            std::strftime(time_str, sizeof(time_str), "%c", std::localtime(&last_modified));
            restarts << time_str << "," << pid << ",RESTARTED" << std::endl;
            restarts.flush();
        }

        // Get CPU usage
        if (++cpuCounter >= cpuInterval)
        {
            double cpuUsage = getCPUUsage(pid);
            if (pid == -1)
            {
                cerr << "Error: Failed to read CPU usage for process " << processName << std::endl;
            }
            else
            {
                writeOutputLine(cpu_file, last_modified, pid, cpuUsage, csvFormat);
            }
            cpuCounter = 0;
        }

        // Get memory usage
        if (++memCounter >= memInterval)
        {
            vector<double> memUsage = getMemUsage(pid);
            if (pid == -1)
            {
                cerr << "Error: Failed to read memory usage for process " << processName << std::endl;
                memUsage = {0, 0, 0};
            }
            else
            {
                writeOutputLine(mem_file, last_modified, pid, memUsage, csvFormat);
            }

            memCounter = 0;
        }

        if (!process_found)
        {
            pid = findProcess(processName);
            if (pid != -1)
            {
                last_modified = time(NULL);
                cpu_file << ctime(&last_modified) << "\t" << pid << "\tFOUND\n";
                cpu_file.flush();
                mem_file << ctime(&last_modified) << "\t" << pid << "\tFOUND\n";
                mem_file.flush();
                process_found = true;
            }
        }

        sleep(1);
    }
}

int main(int argc, char *argv[])
{
    int cpuInterval = 1;
    int memInterval = 5;
    vector<std::string> processNames;
    std::string outputDir = ".", outputFormat = "csv";
    int opt;
    bool csvFormat;

    while ((opt = getopt(argc, argv, "p:d:c:m:f:")) != -1)
    {
        switch (opt)
        {
        case 'p':
            processNames.push_back(optarg);
            break;
        case 'd':
            outputDir = optarg;
            break;
        case 'c':
            cpuInterval = stoi(optarg);
            break;
        case 'm':
            memInterval = stoi(optarg);
            break;
        case 'f':
            outputFormat = optarg;
            break;
        default:
            cerr << "Invalid argument" << std::endl;
            return 1;
        }
    }

    if (processNames.empty())
    {
        cerr << "Usage: " << argv[0] << " -p <processName> -d <outputDir> [-c <cpuInterval>] [-m <memInterval>] [-f <outputFormat>]" << std::endl;
        return 1;
    }

    if (access(outputDir.c_str(), W_OK) == -1)
    {
        cerr << "Output directory is not writable" << std::endl;
        return 1;
    }

    if (outputFormat == "json")
    {
        csvFormat = false;
    }
    else if (outputFormat == "csv")
    {
        csvFormat = true;
    }
    else
    {
        cerr << "Invalid output format. Only 'json' and 'csv' are supported" << std::endl;
        return 1;
    }
    vector<thread> threads;

    std::cout << "Monitoring processes: ";
    for (auto processName : processNames)
    {
        std::cout << processName << " ";
        // Find the process ID of the given process name
        pid_t pid = findProcess(processName);
        if (pid == -1)
        {
            cerr << "Error: Process " << processName << " not found" << std::endl;
        }

        // Create a new thread for monitoring the process
        threads.emplace_back([processName, cpuInterval, memInterval, outputDir, csvFormat]()
                             { monitorProcess(processName, cpuInterval, memInterval, outputDir, csvFormat); });
    }
    std::cout << std::endl;

    std::cout << "Output directory: " << outputDir << std::endl;
    std::cout << "CPU usage interval: " << cpuInterval << " seconds" << std::endl;
    std::cout << "Memory usage interval: " << memInterval << " seconds" << std::endl;
    std::cout << "Output format: " << outputFormat << std::endl;

    // Wait for all threads to complete
    for (auto &thread : threads)
    {
        thread.join();
    }

    return 0;
}