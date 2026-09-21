// mcppls: the language server and its command line.
import std;
import mcppls.cli.commands;

int main(int argc, char* argv[]) {
    return mcppls::cli::run(argc, argv);
}
