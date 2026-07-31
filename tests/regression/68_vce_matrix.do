* 68_vce_matrix — the unified VCE module (DESIGN_VCE.md, v1.6.33).
* Locks the estimator x {default, robust, cluster} matrix cross-rig,
* including Bug 39's fix: xtreg,fe robust PROMOTES to cluster(panelvar)
* (Stata semantics since Stata 10; Stock-Watson 2008), so those two
* rows must be IDENTICAL.  Companion tools/stata_check_vce.do runs the
* same matrix in Stata to pin the policy table to 6 digits.
sysuse grunfeld, clear
quietly xtset firm year
quietly regress invest value capital
display round(_se[value], 1e-6)
quietly regress invest value capital, robust
display round(_se[value], 1e-6)
quietly regress invest value capital, vce(cluster firm)
display round(_se[value], 1e-6)
quietly ivregress 2sls invest (value = capital)
display round(_se[value], 1e-6)
quietly ivregress 2sls invest (value = capital), robust
display round(_se[value], 1e-6)
quietly ivregress 2sls invest (value = capital), vce(cluster firm)
display round(_se[value], 1e-6)
quietly xtreg invest value, fe
display round(_se[value], 1e-6)
quietly xtreg invest value, fe robust
display round(_se[value], 1e-6)
quietly xtreg invest value, fe vce(cluster firm)
display round(_se[value], 1e-6)
sysuse nmes1988, clear
quietly poisson visits chronic school
display round(_se[chronic], 1e-6)
quietly poisson visits chronic school, vce(robust)
display round(_se[chronic], 1e-6)
quietly poisson visits chronic school, vce(cluster region)
display round(_se[chronic], 1e-6)
quietly logit insurance school income
display round(_se[school], 1e-6)
quietly logit insurance school income, vce(robust)
display round(_se[school], 1e-6)
