within ThermoSysPro.Properties.WaterSolution;
function MassFraction_eq_PT "Equilibrium mass fraction of the H2O/LiBr solution as a function of T et Xh2o"
  input ThermoSysPro.Units.AbsolutePressure P "Pressure";
  input ThermoSysPro.Units.AbsoluteTemperature T "Temperature";
  output Real Xe "Equilibrium mass fraction";
protected
  Real lnP "ln de la pression en Pa";
  Real lnPlim "limite des zones du ln de la pression en Pa";
  Real Tinv "Inverse n�gatif de la temp�rature";
  Real A1 "Coefficient directeur zone inf�rieure";
  Real B1 "Ordonn�e � l'origine zone inf�rieure";
  Real A2 "Coefficient directeur zone sup�rieure";
  Real B2 "Ordonn�e � l'origine zone sup�rieure";
  Real a "Coefficient directeur de la loi ln P lim = a (-1/T) + b";
  Real b "Ordonn�e � l'origine de la loi ln P lim = a (-1/T) + b";
algorithm
  lnP:=ln(P);
  Tinv:=-1/T;
  A1:=7058.50237*Tinv*Tinv + 72.9531684*Tinv + 0.264270714;
  B1:=-219138.115*Tinv*Tinv - 2185.32823*Tinv - 5.01454826;
  A2:=11272.3416*Tinv*Tinv - 13.4083981*Tinv + 0.463220115;
  B2:=349286.405*Tinv*Tinv - 415.474563*Tinv - 9.41938792;
  a:=5379.103071;
  b:=25.44182656;
  lnPlim:=a*Tinv + b;
  if lnP < lnPlim then
    Xe:=A1*lnP + B1;
  else
    Xe:=A2*lnP + B2;
  end if;
  annotation(smoothOrder=2, Documentation(info="<html>
<p><b>Copyright &copy; EDF 2002 - 2010</b></p>
</HTML>
<html>
<p><b>ThermoSysPro Version 2.0</b></p>
</HTML>
"));
end MassFraction_eq_PT;
