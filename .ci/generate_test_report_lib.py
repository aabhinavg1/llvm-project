# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Library to parse JUnit XML files and return a markdown report."""

from junitparser import JUnitXml, Failure

SEE_BUILD_FILE_STR = "Download the build's log file to see the details."
UNRELATED_FAILURES_STR = (
    "If these failures are unrelated to your changes (for example "
    "tests are broken or flaky at HEAD), please open an issue at "
    "https://github.com/llvm/llvm-project/issues and add the "
    "`infrastructure` label."
)


# Set size_limit to limit the byte size of the report. The default is 1MB as this
# is the most that can be put into an annotation. If the generated report exceeds
# this limit and failures are listed, it will be generated again without failures
# listed. This minimal report will always fit into an annotation.
# If include failures is False, total number of test will be reported but their names
# and output will not be.
def generate_report(
    title,
    return_code,
    junit_objects,
    size_limit=1024 * 1024,
    list_failures=True,
):
    failures = {}
    tests_run = 0
    tests_skipped = 0
    tests_failed = 0

    for results in junit_objects:
        for testsuite in results:
            tests_run += testsuite.tests
            tests_skipped += testsuite.skipped
            tests_failed += testsuite.failures

            for test in testsuite:
                if (
                    not test.is_passed
                    and test.result
                    and isinstance(test.result[0], Failure)
                ):
                    if failures.get(testsuite.name) is None:
                        failures[testsuite.name] = []
                    failures[testsuite.name].append(
                        (test.classname + "/" + test.name, test.result[0].text)
                    )

    report = [f"# {title}", ""]

    if tests_run == 0:
        if return_code == 0:
            report.extend(
                [
                    "The build succeeded and no tests ran. This is expected in some "
                    "build configurations."
                ]
            )
        else:
            report.extend(
                [
                    "The build failed before running any tests.",
                    "",
                    SEE_BUILD_FILE_STR,
                    "",
                    UNRELATED_FAILURES_STR,
                ]
            )
        return "\n".join(report)

    tests_passed = tests_run - tests_skipped - tests_failed

    def plural(num_tests):
        return "test" if num_tests == 1 else "tests"

    if tests_passed:
        report.append(f"* {tests_passed} {plural(tests_passed)} passed")
    if tests_skipped:
        report.append(f"* {tests_skipped} {plural(tests_skipped)} skipped")
    if tests_failed:
        report.append(f"* {tests_failed} {plural(tests_failed)} failed")

    if not list_failures:
        report.extend(
            [
                "",
                "Failed tests and their output was too large to report. "
                + SEE_BUILD_FILE_STR,
            ]
        )
    elif failures:
        report.extend(
            ["", "## Failed Tests", "(click on a test name to see its output)"]
        )

        for testsuite_name, failures in failures.items():
            report.extend(["", f"### {testsuite_name}"])
            for name, output in failures:
                report.extend(
                    [
                        "<details>",
                        f"<summary>{name}</summary>",
                        "",
                        "```",
                        output,
                        "```",
                        "</details>",
                    ]
                )
    elif return_code != 0:
        # No tests failed but the build was in a failed state. Bring this to the user's
        # attention.
        report.extend(
            [
                "",
                "All tests passed but another part of the build **failed**.",
                "",
                SEE_BUILD_FILE_STR,
            ]
        )

    if failures or return_code != 0:
        report.extend(["", UNRELATED_FAILURES_STR])

    report = "\n".join(report)
    if len(report.encode("utf-8")) > size_limit:
        return generate_report(
            title,
            return_code,
            junit_objects,
            size_limit,
            list_failures=False,
        )

    return report


def generate_report_from_files(title, return_code, junit_files):
    return generate_report(
        title,
        return_code,
        [JUnitXml.fromfile(p) for p in junit_files],
    )
