// Self-contained native (C++ API) port of repro.py's concurrent ALTER/DROP
// COLUMN vs INSERT race. Exists because neither the CLI (single-writer file
// lock serializes separate processes) nor a sqllogictest concurrentloop
// port (independent per-thread loops, no per-round synchronization, no
// retry-on-conflict) reproduced the crash repro.py hits ~80% of the time.
// This mirrors repro.py's structure exactly: fresh threads spawned and
// joined each round, each statement retried up to 10x on the same conflict
// exception types repro.py's run_query() catches, stress run in a forked
// child so a segfault there doesn't take down the reattach check.
#include "duckdb.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

using duckdb::Connection;
using duckdb::DuckDB;
using duckdb::ExceptionType;
using duckdb::string;

static const int ROUNDS = 25;

static bool IsExpectedConflict(const ExceptionType &type) {
	return type == ExceptionType::TRANSACTION || type == ExceptionType::BINDER || type == ExceptionType::CATALOG;
}

static void RunQuery(Connection &con, const string &sql) {
	for (int i = 0; i < 10; i++) {
		auto result = con.Query(sql);
		if (!result->HasError()) {
			return;
		}
		if (!IsExpectedConflict(result->GetErrorObject().Type())) {
			fprintf(stderr, "unexpected error running [%s]: %s\n", sql.c_str(), result->GetError().c_str());
			return;
		}
		// expected write-write / catalog conflict, retry
	}
}

static void AlterQueries(Connection &con) {
	// dbt column-rename pattern; all 3 statements needed to reproduce
	RunQuery(con, "ALTER TABLE t ADD COLUMN IF NOT EXISTS int_col__tmp INTEGER");
	RunQuery(con, "UPDATE t SET int_col__tmp = int_col");
	RunQuery(con, "ALTER TABLE t DROP COLUMN int_col__tmp");
}

static void InsertQuery(Connection &con) {
	RunQuery(con, "INSERT INTO t BY NAME SELECT "
	              "1::INTEGER AS int_col, "
	              "'x' AS varchar_col, "
	              "FROM range(10)");
}

// Runs the concurrent stress test and never returns (always _exit()s). Run
// in a forked child so a segfault here can be observed via its exit status
// instead of taking down the process that still needs to do the reattach
// check.
[[noreturn]] static void RunStress(const string &db_path) {
	DuckDB db(db_path);
	Connection setup(db);
	auto res = setup.Query("CREATE TABLE t AS SELECT "
	                        "range::INTEGER AS int_col, "
	                        "md5(range::VARCHAR)::VARCHAR AS varchar_col, "
	                        "FROM range(200)");
	if (res->HasError()) {
		fprintf(stderr, "setup failed: %s\n", res->GetError().c_str());
		_exit(1);
	}

	Connection conn_a(db);
	Connection conn_b(db);
	for (int r = 0; r < ROUNDS; r++) {
		std::thread ta(AlterQueries, std::ref(conn_a));
		std::thread tb(InsertQuery, std::ref(conn_b));
		ta.join();
		tb.join();
	}
	printf("child: completed %d rounds without crash\n", ROUNDS);
	_exit(0);
}

int main() {
	printf("duckdb version: %s (%s)\n", DuckDB::LibraryVersion(), DuckDB::SourceID());

	const char *tmpdir_env = getenv("TMPDIR");
	string tmpdir_base = (tmpdir_env && *tmpdir_env) ? tmpdir_env : "/tmp";
	if (!tmpdir_base.empty() && tmpdir_base.back() == '/') {
		tmpdir_base.pop_back();
	}
	char dir_template[4096];
	snprintf(dir_template, sizeof(dir_template), "%s/native_repro_XXXXXX", tmpdir_base.c_str());
	if (mkdtemp(dir_template) == nullptr) {
		perror("mkdtemp");
		return 1;
	}
	string db_path = string(dir_template) + "/repro.duckdb";

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}
	if (pid == 0) {
		RunStress(db_path); // never returns
	}

	int status = 0;
	waitpid(pid, &status, 0);
	bool crashed = false;
	if (WIFSIGNALED(status)) {
		crashed = true;
		printf("CHILD CRASHED: killed by signal %d (%s)\n", WTERMSIG(status), strsignal(WTERMSIG(status)));
	} else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		crashed = true;
		printf("CHILD FAILED: exit code %d\n", WEXITSTATUS(status));
	}

	// reattach check: does the DB (incl. WAL) replay cleanly? A corrupt WAL
	// throws out of the DuckDB constructor itself (during replay), not out
	// of a query, hence the try/catch here rather than a HasError() check.
	bool reattach_error = false;
	try {
		DuckDB db(db_path);
		Connection con(db);
		auto res = con.Query("SELECT count(*) FROM t");
		if (res->HasError()) {
			reattach_error = true;
			printf("REATTACH FAILED: %s\n", res->GetError().c_str());
		} else {
			printf("reattach OK, t has %s rows\n", res->GetValue(0, 0).ToString().c_str());
		}
	} catch (std::exception &e) {
		reattach_error = true;
		printf("REATTACH FAILED: %s\n", e.what());
	}

	std::error_code ec;
	std::filesystem::remove_all(dir_template, ec);

	if (crashed || reattach_error) {
		printf("RESULT: FAIL (bug reproduced)\n");
		return 1;
	}
	printf("RESULT: PASS (no corruption observed)\n");
	return 0;
}
