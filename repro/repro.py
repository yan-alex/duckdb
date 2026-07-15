import os
import shutil
import subprocess
import sys
import tempfile
import threading

import duckdb

ROUNDS = 25


def run_query(cur, sql):
    for _ in range(10):
        try:
            return cur.execute(sql)
        except (duckdb.TransactionException, duckdb.BinderException,
                duckdb.CatalogException):
            pass  # expected write-write / catalog conflicts


def alter_queries(conn):
    # dbt column-rename pattern; all 3 statements needed to reproduce
    run_query(conn, "ALTER TABLE t ADD COLUMN IF NOT EXISTS int_col__tmp INTEGER")
    run_query(conn, "UPDATE t SET int_col__tmp = int_col")
    run_query(conn, "ALTER TABLE t DROP COLUMN int_col__tmp")


def insert_query(conn):
    run_query(conn, "INSERT INTO t BY NAME SELECT "
                    "1::INTEGER AS int_col, "
                    "'x' AS varchar_col, "
                    "FROM range(10)")


def child(db):
    duckdb.connect(db).execute(
        "CREATE TABLE t AS SELECT "
        "range::INTEGER AS int_col, "
        # type holding pointers to data is needed to repro
        "md5(range::VARCHAR)::VARCHAR AS varchar_col, "
        "FROM range(200)")
    conn_a, conn_b = duckdb.connect(db), duckdb.connect(db)
    for r in range(ROUNDS):
        threads = [threading.Thread(target=alter_queries, args=(conn_a,)),
                   threading.Thread(target=insert_query, args=(conn_b,))]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    print(f"child: completed {ROUNDS} rounds without crash")


def main():
    print(f"duckdb version: {duckdb.__version__}")
    tmpdir = tempfile.mkdtemp(prefix="alter_dml_")
    db = os.path.join(tmpdir, "repro.duckdb")
    try:
        # run the stress in a child process: it may segfault
        proc = subprocess.run([sys.executable, __file__, "--child", db],
                              capture_output=True, text=True, timeout=600)
        crashed = proc.returncode != 0
        print(proc.stdout[-2000:])
        if crashed:
            print(f"CHILD CRASHED: exit code {proc.returncode}")

        # reattach check: does the DB (incl. WAL) replay cleanly?
        reattach_error = None
        try:
            con = duckdb.connect(db)
            n = con.execute("SELECT count(*) FROM t").fetchone()[0]
            print(f"reattach OK, t has {n} rows")
            con.close()
        except Exception as e:
            reattach_error = str(e)
            print(f"REATTACH FAILED: {reattach_error}")

        if crashed or reattach_error:
            print("RESULT: FAIL (bug reproduced)")
            sys.exit(1)
        print("RESULT: PASS (no corruption observed)")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--child":
        child(sys.argv[2])
    else:
        main()