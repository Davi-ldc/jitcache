# Sources of the PDF inputs

`inputs/pdf` holds six documents of the US federal government, 3.7 MB in all, each text-based and of a different kind. A work that an officer or employee of the US government prepares as part of their official duties has no copyright in the United States (17 U.S.C. § 105). They were downloaded on 2026-10-13, and five are copied unchanged.

| file | document | pages | source | why it is public domain |
|---|---|---|---|---|
| `bea-personal-income-2025-08.pdf` | "Personal Income and Outlays, August 2025", news release BEA 25-41 of the Bureau of Economic Analysis, September 26, 2025: text and tables | 13 | https://www.bea.gov/sites/default/files/2025-09/pi0825.pdf | a release written by BEA staff, 17 U.S.C. § 105 |
| `census-acsbr-025.pdf` | "Household Income in States and Metropolitan Areas: 2024", American Community Survey Brief ACSBR-025 of the US Census Bureau, by Gloria G. Guzman, September 2025: a statistical report with tables and maps | 10 | https://www2.census.gov/library/publications/2025/demo/acsbr-025.pdf | a report written by a Census Bureau employee, 17 U.S.C. § 105 |
| `federal-register-2025-23783.pdf` | Federal Register document 2025-23783, Vol. 90, No. 244, December 23, 2025, page 59945 on: the Department of Homeland Security's final rule "Privacy Act of 1974" (6 CFR Part 5), in the Register's three columns; its last page also starts a Federal Housing Finance Agency rule | 4 | https://www.govinfo.gov/content/pkg/FR-2025-12-23/pdf/2025-23783.pdf | a rule written by DHS and published by the Office of the Federal Register, 17 U.S.C. § 105 |
| `irs-form-w-9.pdf` | Form W-9 (Rev. March 2024), "Request for Taxpayer Identification Number and Certification", of the Internal Revenue Service: a fillable form with its instructions | 6 | https://www.irs.gov/pub/irs-pdf/fw9.pdf | a form written by the IRS, 17 U.S.C. § 105 |
| `nist-sp-800-218-pages-1-16.pdf` | pages 1 to 16 of NIST Special Publication 800-218, "Secure Software Development Framework (SSDF) Version 1.1", February 2022: the front matter, the introduction and the first three pages of the practices table | 16 of 36 | https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-218.pdf | its page 3 says the publication "is not subject to copyright in the United States" |
| `supreme-court-22-1218.pdf` | the slip opinion of the Supreme Court of the United States in Smith v. Spizzirri, No. 22-1218, decided May 16, 2024 | 8 | https://www.supremecourt.gov/opinions/23pdf/22-1218_5357.pdf | judicial opinions have no copyright under the government edicts doctrine, and the Court's opinion is a work of the federal government, 17 U.S.C. § 105 |

The IRS URL serves the form's current revision, and the file here is the March 2024 one. The NIST excerpt came from qpdf 11.9.0, which copies the pages and the document's metadata without changing their content; with `--deterministic-id` the command below gives the same bytes again:

```sh
qpdf --deterministic-id NIST.SP.800-218.pdf --pages . 1-16 -- nist-sp-800-218-pages-1-16.pdf
```

```
3308842a2c5eb813444b05d83820aed19e8e673cf301e838327ff7fffe4106e3  bea-personal-income-2025-08.pdf
5de1b0d5ec469344896bdf8485d3e93206351c0f1f9e51029b8d57d2df9054b9  census-acsbr-025.pdf
85a83cfaa8b688a5a16b1ecb28ff9109304ee7f9ad77b36186991806dfc976b0  federal-register-2025-23783.pdf
2d420cbb4123dcf1fb82595b2359cfbb5d81f00b9df9d359fcc7af361d093f53  irs-form-w-9.pdf
298697e0a140e0fa4a3f026b282dc873a03f18575935cc8a25747044b177e4ad  nist-sp-800-218-pages-1-16.pdf
f21d3b8e0f0759fb3314a7ac847a7cc0e7dad69c2e83ec7caf36961634e8bb01  supreme-court-22-1218.pdf
```

Those are the files' SHA-256 sums; the full NIST SP 800-218 from the URL above has `617746e553a9e2da49bfbd4eef0dfc3094758a39b869314e4173ac36605cde22`.

`package.json` pins each library's exact version, and `bun.lock` pins the rest, among them Elysia's peers `@sinclair/typebox` 0.34.52, `file-type` and `openapi-types`, and `jose` 6.2.12, which `@elysiajs/jwt` signs and verifies with.
