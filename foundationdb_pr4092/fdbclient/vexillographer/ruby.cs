/*
 * ruby.cs
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace vexillographer
{
    class ruby : BindingWriter
    {
		private static Dictionary<ParamType, String> typeMap = new Dictionary<ParamType, String>()
		{
			{ ParamType.None, "nil" },
			{ ParamType.Int, "0" },
			{ ParamType.String, "''" },
            { ParamType.Bytes, "''" }
		};

        private static string getRubyLine(Option o)
        {
			return String.Format("    \"{0}\" => [{1}, \"{2}\", {3}, {4}],", o.name.ToUpper(), o.code, o.comment, typeMap[o.paramType], o.paramDesc == null ? "nil" : "\"" + o.paramDesc + "\"");
        }

        private static void writeRubyHash(TextWriter outFile, Scope scope, IEnumerable<Option> options)
        {
            outFile.WriteLine("  @@{0} = {{", scope.ToString());
            outFile.WriteLine(string.Join("\n", options.Where(f => !f.hidden).Select(f => getRubyLine(f)).ToArray()));
            outFile.WriteLine("  }");
            outFile.WriteLine();
        }

        public void writeFiles(string filePath, IEnumerable<Option> options)
        {
            using (var rbFile = System.IO.File.Open(filePath,
                                                    System.IO.FileMode.Create,
                                                    System.IO.FileAccess.Write))
            {
                TextWriter outFile = new StreamWriter(rbFile);
                outFile.NewLine = "\n";
				outFile.WriteLine(@"# FoundationDB Ruby API
# Copyright (c) 2013-2017 Apple Inc.

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the ""Software""), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED ""AS IS"", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

# Documentation for this API can be found at
# https://apple.github.io/foundationdb/api-ruby.html

module FDB");
                foreach (Scope s in Enum.GetValues(typeof(Scope)))
                {
                    writeRubyHash(outFile, s, options.Where(o => o.scope == s && o.scope != Scope.ErrorPredicate));
                }
                outFile.WriteLine("end");
                outFile.Flush();
            }
        }
    }
}
